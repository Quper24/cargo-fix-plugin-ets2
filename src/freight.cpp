#include "freight.h"
#include "generation_limit.h"
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
using Generate = void(__fastcall*)(std::uintptr_t, std::uintptr_t, std::uint32_t, std::uint64_t, bool);
using Schedule = void(__fastcall*)(std::uintptr_t, std::uintptr_t, std::uint32_t, std::uintptr_t, std::uint32_t);
using ClearCompany = void(__fastcall*)(std::uintptr_t, std::uintptr_t);
using Reserve = bool(__fastcall*)(Vector*, std::uint64_t);
using Append = bool(__fastcall*)(Vector*, std::uint64_t);
using Create = std::uintptr_t(__fastcall*)(std::uintptr_t);
using ClearVector = void(__fastcall*)(Vector*);

std::uintptr_t base = 0;
Logger logger = nullptr;
GenerationLimit limit;
unsigned hotkey = VK_F9;
bool initialized = false, key_down = false, reconcile = true;
std::uintptr_t manual_company = 0;
std::uint64_t manual_index = 0;
constexpr std::uint64_t MaxSlots = 512;

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
Hook generate_hook, schedule_hook, clear_hook;

// The base market uses a separate offer vector at company+0x78. Its slot
// descriptors belong to company+0x10 and must grow with the runtime offers.
bool EnsureDescriptors(std::uintptr_t company, std::uint64_t needed) {
    if (!needed || needed > MaxSlots) return false;
    const auto definition = At<std::uintptr_t>(company, 0x10);
    if (!definition) return false;
    auto* flags = reinterpret_cast<Vector*>(definition + 0x90);
    if (!flags->data || !flags->count || flags->count > MaxSlots || !flags->vtable) return false;
    const auto original_count = flags->count;
    if (flags->count < needed) {
        if (flags->capacity < needed &&
            !At<Reserve>(flags->vtable, 0x18)(flags, needed)) return false;
        if (!flags->data || flags->capacity < needed) return false;
        auto* data = reinterpret_cast<std::uint32_t*>(flags->data);
        for (auto n = flags->count; n < needed; ++n) data[n] = data[n % original_count];
        flags->count = needed;
    }
    if (At<unsigned char>(definition, 0xc4) & 2) {
        auto* parking = reinterpret_cast<Vector*>(definition + 0xd0);
        if (parking->count > MaxSlots) return false;
        if (parking->count < needed &&
            !reinterpret_cast<Append>(base + 0xf36c0)(parking, needed - parking->count)) return false;
    }
    auto& configured_count = At<std::uint32_t>(definition, 0xc0);
    configured_count = (std::max)(configured_count, static_cast<std::uint32_t>(needed));
    return true;
}
struct DescriptorSnapshot {
    std::uintptr_t definition;
    std::uint64_t flags_count, parking_count;
    std::uint32_t configured_count;
    explicit DescriptorSnapshot(std::uintptr_t value) : definition(value),
        flags_count(At<std::uint64_t>(value, 0xa0)),
        parking_count(At<std::uint64_t>(value, 0xe0)),
        configured_count(At<std::uint32_t>(value, 0xc0)) {}
    void Rollback() const {
        if (At<unsigned char>(definition, 0xc4) & 2) {
            auto* parking = reinterpret_cast<Vector*>(definition + 0xd0);
            using ClearParkingHandle = void(__fastcall*)(std::uintptr_t);
            for (auto n = parking_count; n < parking->count; ++n)
                reinterpret_cast<ClearParkingHandle>(base + 0x1086f0)(parking->data + n * 8);
            parking->count = parking_count;
        }
        At<std::uint64_t>(definition, 0xa0) = flags_count;
        At<std::uint32_t>(definition, 0xc0) = configured_count;
    }
};
void ReleaseJob(std::uintptr_t job) {
    if (!job) return;
    Vector single{0, reinterpret_cast<std::uintptr_t>(&job), 1, 1};
    reinterpret_cast<ClearVector>(base + 0x116510)(&single);
}

void __fastcall ScheduleBridge(std::uintptr_t scheduler, std::uintptr_t economy, std::uint32_t time,
                               std::uintptr_t company, std::uint32_t slot) {
    if (manual_company == company && manual_index == slot) return;
    schedule_hook.Original<Schedule>()(scheduler, economy, time, company, slot);
}
void __fastcall GenerateBridge(std::uintptr_t company, std::uintptr_t economy, std::uint32_t time,
                               std::uint64_t slot, bool forced) {
    if (!company || !economy) return;
    auto* jobs = reinterpret_cast<Vector*>(company + 0x78);
    if (slot >= jobs->count || !jobs->data) return;
    const auto definition = At<std::uintptr_t>(company, 0x10);
    if (jobs->count > MaxSlots || !EnsureDescriptors(company, jobs->count)) {
        if (!definition || slot < At<std::uint64_t>(definition, 0xa0))
            generate_hook.Original<Generate>()(company, economy, time, slot, forced);
        return;
    }
    generate_hook.Original<Generate>()(company, economy, time, slot, forced);
}
void __fastcall ClearBridge(std::uintptr_t company, std::uintptr_t economy) {
    if (company) {
        auto* jobs = reinterpret_cast<Vector*>(company + 0x78);
        if (jobs->count && jobs->count <= MaxSlots && !EnsureDescriptors(company, jobs->count)) {
            // If descriptors cannot grow, keep the stock clear path within its
            // original parking-array bounds and release the tail via game code.
            const auto definition = At<std::uintptr_t>(company, 0x10);
            std::uint64_t allowed = definition ? At<std::uint64_t>(definition, 0xa0) : 0;
            if (definition && (At<unsigned char>(definition, 0xc4) & 2))
                allowed = (std::min)(allowed, At<std::uint64_t>(definition, 0xe0));
            allowed = (std::min)(allowed, jobs->count);
            auto* data = reinterpret_cast<std::uintptr_t*>(jobs->data);
            for (auto i = allowed; i < jobs->count; ++i) { ReleaseJob(data[i]); data[i] = 0; }
            jobs->count = allowed;
        }
    }
    clear_hook.Original<ClearCompany>()(company, economy);
}

void ShowCooldown(std::uint32_t now) {
    const auto minutes = limit.NextAvailable() - now;
    wchar_t message[400];
    const auto format = notifications::Translate(L"FreightCooldown",
        L"Freight market / Quick jobs\nGeneration limit reached. Try again in %u h %02u min of game time.",
        L"Рынок грузоперевозок / Заказы агентств\nЛимит генерации исчерпан. Повторная генерация через %u ч %02u мин игрового времени.");
    swprintf_s(message, format.c_str(), minutes / 60, minutes % 60);
    notifications::Show(message);
}
void Run() {
    if (!initialized) return;
    const auto economy = At<std::uintptr_t>(base, 0x3046780);
    if (!economy) return;
    const auto now = At<std::uint32_t>(economy, 0x19c);
    limit.Refresh(now);
    if (limit.Blocked()) { ShowCooldown(now); return; }
    const auto array = At<std::uintptr_t>(economy, 0x28);
    const auto count = At<std::uint64_t>(economy, 0x30);
    if (!array || !count || count > 10000) return;
    const auto allocator_index = At<std::uint32_t>(base, 0x2733298);
    if (allocator_index >= 4) {
        Log("Freight generation cancelled: allocator unavailable.");
        notifications::Show(notifications::Translate(L"FreightNotReady",
            L"Freight generation is not ready in the current game state. Try again after returning to driving.",
            L"Генерация пока недоступна в текущем состоянии игры. Попробуйте снова после возвращения в кабину."));
        return;
    }
    const auto allocator = At<std::uintptr_t>(base + 0x367f918, allocator_index * 8);
    if (!allocator) return;
    const auto deadline = limit.Deadline(now);
    const auto started = GetTickCount64();
    std::uint64_t added = 0, complete = 0, eligible = 0;
    notifications::Show(notifications::Translate(L"FreightGenerating",
        L"Generating Freight market / Quick jobs offers…",
        L"Генерация заказов для рынка грузоперевозок и агентств…"));
    Log("Freight / Quick jobs append pass started (stock automatic generation retained).");
    const auto* companies = reinterpret_cast<const std::uintptr_t*>(array);
    for (std::uint64_t n = 0; n < count; ++n) {
        const auto company = companies[n];
        if (!company) continue;
        const auto definition = At<std::uintptr_t>(company, 0x10);
        const auto context = At<std::uintptr_t>(company, 0x68);
        if (!definition || !context || At<std::int32_t>(context, 8) >= 0) continue;
        auto* jobs = reinterpret_cast<Vector*>(company + 0x78);
        if (!jobs->data || !jobs->count || jobs->count > MaxSlots ||
            !EnsureDescriptors(company, jobs->count)) continue;
        ++eligible;
        unsigned company_added = 0;
        for (unsigned offer = 0; offer < 5; ++offer) {
            auto* data = reinterpret_cast<std::uintptr_t*>(jobs->data);
            // Always use a new slot: reusing an old slot could leave a saved
            // native refresh event that overwrites the new offer too early.
            const std::uint64_t slot = jobs->count;
            const DescriptorSnapshot snapshot(definition);
            {
                if (slot >= MaxSlots) break;
                if (!EnsureDescriptors(company, slot + 1)) { snapshot.Rollback(); break; }
                const auto job = reinterpret_cast<Create>(base + 0xaba700)(allocator);
                if (!job) { snapshot.Rollback(); break; }
                auto* refs = reinterpret_cast<volatile LONG*>(job + 8);
                InterlockedExchangeAdd(refs, 0x20000001);
                InterlockedXor(refs, 0x60000000); InterlockedXor(refs, 0x60000000);
                if (!reinterpret_cast<Append>(base + 0xf36c0)(jobs, 1)) {
                    ReleaseJob(job); snapshot.Rollback(); break;
                }
                data = reinterpret_cast<std::uintptr_t*>(jobs->data);
                data[slot] = job;
            }
            bool created = false;
            manual_company = company; manual_index = slot;
            for (unsigned attempt = 0; attempt < 20; ++attempt) {
                GenerateBridge(company, economy, now, slot, true);
                data = reinterpret_cast<std::uintptr_t*>(jobs->data);
                if (slot >= jobs->count || !data[slot]) break;
                const auto job = data[slot];
                if (At<std::uintptr_t>(job, 0x58)) {
                    At<std::uint32_t>(job, 0x48) = deadline;
                    At<std::uint32_t>(job, 0x60) = deadline;
                    created = true; break;
                }
            }
            manual_company = 0;
            if (!created) {
                if (slot + 1 == jobs->count) {
                    data = reinterpret_cast<std::uintptr_t*>(jobs->data);
                    ReleaseJob(data[slot]); data[slot] = 0; --jobs->count;
                }
                snapshot.Rollback();
                break;
            }
            schedule_hook.Original<Schedule>()(At<std::uintptr_t>(economy, 0x168), economy,
                deadline, company, static_cast<std::uint32_t>(slot));
            ++company_added; ++added;
        }
        if (company_added == 5) ++complete;
    }
    if (added) limit.RecordSuccess(now);
    wchar_t message[420];
    const auto format = notifications::Translate(L"FreightResult",
        L"Freight market / Quick jobs\nOrders added: %llu. Available runs: %u of 3.",
        L"Рынок грузоперевозок / Заказы агентств\nДобавлено заказов: %llu. Доступно запусков: %u из 3.");
    swprintf_s(message, format.c_str(), added, limit.RemainingUses());
    std::wstring result = message;
    if (complete < eligible) result += notifications::Translate(L"FreightPartial",
        L"\nSome companies do not have enough suitable cargo.",
        L"\nНе во всех компаниях достаточно подходящих грузов.");
    if (!added) result += notifications::Translate(L"FreightNotConsumed",
        L"\nThis generation slot was not consumed.", L"\nЗапуск не израсходован.");
    notifications::Show(result);
    char log[260];
    sprintf_s(log, "Freight / Quick jobs append pass finished: added=%llu, five_offers=%llu/%llu, remaining=%u, elapsed_ms=%llu.",
        added, complete, eligible, limit.RemainingUses(), GetTickCount64() - started);
    Log(log);
}
}

bool Initialize(std::uintptr_t module, const std::filesystem::path& directory, Logger log) {
    base = module; logger = log;
    hotkey = GetPrivateProfileIntW(L"General", L"FreightHotkeyVirtualKey", VK_F9,
                                  (directory / L"CargoControlETS2.ini").c_str());
    if (!hotkey || hotkey > 255) hotkey = VK_F9;
    if (!schedule_hook.Install(0x81cec9, {0x48,0x89,0x5c,0x24,0x10}, reinterpret_cast<void*>(&ScheduleBridge)) ||
        !clear_hook.Install(0x7d0ce0, {0x48,0x89,0x5c,0x24,0x08}, reinterpret_cast<void*>(&ClearBridge)) ||
        !generate_hook.Install(0x7cded0, {0x4c,0x89,0x4c,0x24,0x20}, reinterpret_cast<void*>(&GenerateBridge))) {
        generate_hook.Remove(); clear_hook.Remove(); schedule_hook.Remove();
        Log("Freight mode disabled: additional native signatures/hooks failed validation.");
        return false;
    }
    initialized = true; Reset();
    Log("Freight / Quick jobs mode enabled; separate three-slot game-time limit.");
    return true;
}
void Reset() { limit.Reset(); key_down = false; reconcile = true; }
void PrepareForDriving() { reconcile = true; }
void Tick(bool driving, unsigned cargo_key) {
    const bool down = (GetAsyncKeyState(hotkey) & 0x8000) != 0;
    const bool pressed = down && !key_down; key_down = down;
    DWORD foreground_process = 0; GetWindowThreadProcessId(GetForegroundWindow(), &foreground_process);
    const bool requested = driving && pressed && foreground_process == GetCurrentProcessId();
    if (!initialized) {
        if (requested) notifications::Show(notifications::Translate(L"FreightDisabled",
            L"Freight mode is unavailable. Check game.log.txt for the initialization error.",
            L"Новый режим недоступен. Причина ошибки инициализации указана в game.log.txt."));
        return;
    }
    if (reconcile) {
        const auto economy = At<std::uintptr_t>(base, 0x3046780);
        if (economy) {
            const auto array = At<std::uintptr_t>(economy, 0x28);
            const auto count = At<std::uint64_t>(economy, 0x30);
            if (array && count && count <= 10000) {
                const auto* companies = reinterpret_cast<const std::uintptr_t*>(array);
                bool any_loaded = false;
                for (std::uint64_t i = 0; i < count; ++i) if (companies[i]) {
                    const auto slots = At<std::uint64_t>(companies[i], 0x88);
                    if (slots) { any_loaded = true; EnsureDescriptors(companies[i], slots); }
                }
                reconcile = !any_loaded;
            }
        }
    }
    if (requested && hotkey == cargo_key) {
        notifications::Show(notifications::Translate(L"KeyConflict",
            L"Both modes use the same key. Assign different hotkeys in CargoControlETS2.ini.",
            L"У режимов совпадают горячие клавиши. Назначьте разные клавиши в CargoControlETS2.ini."));
    } else if (requested) Run();
}
void Shutdown() {
    initialized = false;
    generate_hook.Remove(); clear_hook.Remove(); schedule_hook.Remove();
    Reset(); manual_company = 0;
}
}
