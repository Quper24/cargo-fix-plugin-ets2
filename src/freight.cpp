#include "freight.h"
#include "company_list.h"
#include "refresh_cooldown.h"
#include "notifications.h"
#include <Windows.h>
#include <array>
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <string>

namespace freight {
namespace {
struct Vector {
    std::uintptr_t vtable;
    std::uintptr_t data;
    std::uint64_t count;
    std::uint64_t capacity;
};
static_assert(sizeof(Vector) == 32);
using Schedule = void(__fastcall*)(std::uintptr_t, std::uintptr_t, std::uint32_t, std::uintptr_t, std::uint32_t);

std::uintptr_t base = 0;
const GameProfile* profile = nullptr;
Logger logger = nullptr;
RefreshCooldown cooldown;
bool initialized = false;
std::uintptr_t manual_company = 0;


void Log(const char* text) { if (logger) logger(text); }
template<class T> T& At(std::uintptr_t address, std::uintptr_t offset = 0) {
    return *reinterpret_cast<T*>(address + offset);
}
bool Write(void* address, const void* data, std::size_t length) {
    DWORD previous = 0;
    if (!VirtualProtect(address, length, PAGE_EXECUTE_READWRITE, &previous)) return false;
    std::memcpy(address, data, length);
    FlushInstructionCache(GetCurrentProcess(), address, length);
    DWORD ignored = 0; VirtualProtect(address, length, previous, &ignored);
    return true;
}
void AbsoluteJump(unsigned char* address, std::uintptr_t target) {
    const unsigned char opcode[6] = {0xff, 0x25, 0, 0, 0, 0};
    std::memcpy(address, opcode, 6); std::memcpy(address + 6, &target, 8);
}
struct Hook {
    unsigned char* site = nullptr;
    unsigned char* memory = nullptr;
    std::array<unsigned char, 5> original{};
    bool Install(std::uintptr_t rva, const std::array<unsigned char, 5>& expected, void* bridge) {
        site = reinterpret_cast<unsigned char*>(base + rva);
        if (std::memcmp(site, expected.data(), 5)) return false;
        for (std::uintptr_t offset = 0x4000000; offset < 0x70000000 && !memory; offset += 0x10000)
            memory = static_cast<unsigned char*>(VirtualAlloc(reinterpret_cast<void*>(base + offset),
                4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        if (!memory) return false;
        original = expected;
        AbsoluteJump(memory, reinterpret_cast<std::uintptr_t>(bridge));
        std::memcpy(memory + 32, original.data(), 5);
        AbsoluteJump(memory + 37, reinterpret_cast<std::uintptr_t>(site + 5));
        DWORD previous = 0;
        if (!VirtualProtect(memory, 4096, PAGE_EXECUTE_READ, &previous)) {
            VirtualFree(memory, 0, MEM_RELEASE); memory = nullptr; return false;
        }
        FlushInstructionCache(GetCurrentProcess(), memory, 64);
        unsigned char jump[5] = {0xe9};
        const auto relative = static_cast<std::int32_t>(reinterpret_cast<std::uintptr_t>(memory) - reinterpret_cast<std::uintptr_t>(site + 5));
        std::memcpy(jump + 1, &relative, 4);
        if (!Write(site, jump, 5)) { VirtualFree(memory, 0, MEM_RELEASE); memory = nullptr; return false; }
        return true;
    }
    template<class T> T Original() const { return reinterpret_cast<T>(memory + 32); }
    void Remove() {
        if (!memory) return;
        if (Write(site, original.data(), 5)) { VirtualFree(memory, 0, MEM_RELEASE); memory = nullptr; }
    }
};
Hook schedule_hook;
std::uint32_t manual_deadline = 0;

void __fastcall ScheduleBridge(std::uintptr_t scheduler, std::uintptr_t economy, std::uint32_t time,
                               std::uintptr_t company, std::uint32_t slot) {
    // Native clear removes old company events. Only valid regular offers from
    // the current manual refresh get the requested lifetime; stock retries and
    // special company events retain their native timing.
    if (manual_company == company && !(slot & 0x80000000u)) {
        const auto* jobs = reinterpret_cast<const Vector*>(company + 0x78);
        if (jobs->data && slot < jobs->count) {
            const auto job = reinterpret_cast<const std::uintptr_t*>(jobs->data)[slot];
            if (job && At<std::uintptr_t>(job, 0x58)) time = manual_deadline;
        }
    }
    schedule_hook.Original<Schedule>()(scheduler, economy, time, company, slot);
}

void Run(std::uintptr_t economy) {
    if (!initialized || !economy) return;
    const auto now = At<std::uint32_t>(economy, 0x19c);
    const auto remaining = cooldown.Remaining(GetTickCount64());
    if (remaining) {
        wchar_t message[400];
        const auto format = notifications::Translate(L"FreightCooldown",
            L"Freight market / Quick jobs\nPlease wait %u s before refreshing again.",
            L"Рынок грузоперевозок / Заказы агентств\nПовторное обновление доступно через %u с.");
        swprintf_s(message, format.c_str(), static_cast<unsigned>((remaining + 999) / 1000));
        notifications::Show(message); return;
    }
    const auto array = At<std::uintptr_t>(economy, 0x28);
    const auto count = At<std::uint64_t>(economy, 0x30);
    char list_log[180];
    sprintf_s(list_log, "Freight request: companies=%llu.", count); Log(list_log);
    if (!ValidCompanyList(array, count)) {
        Log("Freight request rejected: company list is empty, unreadable or exceeds safety bound.");
        notifications::Show(notifications::Translate(L"InvalidCompanies",
            L"Company list unavailable. See game.log.txt.",
            L"Список компаний недоступен. Подробности в game.log.txt."));
        return;
    }
    const auto deadline = now + 48u * 60u;
    const auto started = GetTickCount64();
    std::uint64_t refreshed = 0, companies_refreshed = 0, skipped_invalid = 0;
    notifications::Show(notifications::Translate(L"FreightGenerating",
        L"Refreshing Freight market / Quick jobs offers…",
        L"Обновление заказов рынка грузоперевозок и агентств…"));
    Log("Freight / Quick jobs stock refresh started; no slot expansion.");
    const auto* companies = reinterpret_cast<const std::uintptr_t*>(array);
    using RefreshCompany = void(__fastcall*)(std::uintptr_t, std::uintptr_t);
    const auto clear = reinterpret_cast<RefreshCompany>(base + profile->clear_company);
    const auto fill = reinterpret_cast<RefreshCompany>(base + profile->fill_company);
    for (std::uint64_t n = 0; n < count; ++n) {
        const auto company = companies[n];
        if (!company) continue;
        const auto definition = At<std::uintptr_t>(company, 0x10);
        const auto context = At<std::uintptr_t>(company, 0x68);
        if (!definition || !context || At<std::int32_t>(context, 8) >= 0) continue;
        const auto* existing = reinterpret_cast<const Vector*>(company + 0x78);
        // Do not attempt to refresh incompatible saved state produced by 0.4.
        // It must be replaced by a save from before the first F9 use.
        if (At<unsigned char>(definition, 0xc4) & 2) {
            const auto physical_nodes = At<std::uint64_t>(definition, 0x78);
            if (existing->count > physical_nodes ||
                At<std::uint64_t>(definition, 0xe0) > physical_nodes ||
                At<std::uint32_t>(definition, 0xc0) > physical_nodes) {
                ++skipped_invalid; continue;
            }
        }
        manual_company = company; manual_deadline = deadline;
        // Exactly the native stock refresh sequence used by economy_refresh.
        // It removes old offers and their events, then rebuilds the map-defined
        // number of slots. No arrays or map descriptors are expanded by this DLL.
        clear(company, economy);
        fill(company, economy);
        manual_company = 0;
        const auto* jobs = reinterpret_cast<const Vector*>(company + 0x78);
        if (!jobs->data || jobs->count > 10000) continue;
        const auto* data = reinterpret_cast<const std::uintptr_t*>(jobs->data);
        unsigned valid = 0;
        for (std::uint64_t i = 0; i < jobs->count; ++i) {
            const auto job = data[i];
            if (job && At<std::uintptr_t>(job, 0x58)) {
                At<std::uint32_t>(job, 0x48) = deadline;
                At<std::uint32_t>(job, 0x60) = deadline;
                ++valid;
            }
        }
        refreshed += valid;
        if (valid) ++companies_refreshed;
    }
    cooldown.Start(GetTickCount64()); // Cooldown starts after completion, including empty results.
    wchar_t message[420];
    const auto format = notifications::Translate(L"FreightResult",
        L"Freight market / Quick jobs\nOrders refreshed: %llu. You can refresh again in 5 s.",
        L"Рынок грузоперевозок / Заказы агентств\nОбновлено заказов: %llu. Следующее обновление доступно через 5 с.");
    swprintf_s(message, format.c_str(), refreshed);
    std::wstring result = message;
    if (skipped_invalid) result += notifications::Translate(L"FreightInvalidSave",
        L"\nIncompatible slots found. Load a save from before using version 0.4 F9.",
        L"\nНайдены несовместимые слоты. Загрузите сохранение до использования F9 версии 0.4.");
    else if (!refreshed) result += notifications::Translate(L"FreightNotConsumed",
        L"\nNo offers were created.",
        L"\nНовые предложения не созданы.");
    notifications::Show(result);
    char log[280];
    sprintf_s(log, "Freight / Quick jobs stock refresh finished: refreshed=%llu, companies=%llu, skipped_invalid=%llu, cooldown_ms=5000, elapsed_ms=%llu.",
        refreshed, companies_refreshed, skipped_invalid, GetTickCount64() - started);
    Log(log);
}
}

bool Initialize(std::uintptr_t module, const GameProfile& selected, const std::filesystem::path& directory, Logger log) {
    base = module; profile = &selected; logger = log;
    if (!schedule_hook.Install(profile->schedule, {0x48,0x89,0x5c,0x24,0x10}, reinterpret_cast<void*>(&ScheduleBridge))) {
        Log("Freight mode disabled: native schedule signature/hook failed validation.");
        return false;
    }
    initialized = true; Reset();
    Log("Freight / Quick jobs stock refresh enabled; physical slot counts remain native.");
    return true;
}
void Reset() { cooldown.Reset(); }
void PrepareForDriving() {}
void Request(std::uintptr_t economy) {
    if (!initialized) {
        notifications::Show(notifications::Translate(L"FreightDisabled",
            L"Freight mode is unavailable. See game.log.txt.",
            L"Обновление заказов недоступно. Подробности в game.log.txt."));
    } else Run(economy);
}
void Shutdown() {
    initialized = false;
    schedule_hook.Remove();
    Reset(); manual_company = 0;
}
}

