#include <Windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <cstring>
#include <unordered_map>
#include "notifications.h"
#include "generation_limit.h"
#include "freight.h"
#include "company_list.h"
#include "overlay_ui.h"
#include "game_profile.h"

#include "scssdk_telemetry.h"
#include "scssdk_telemetry_event.h"
#include "eurotrucks2/scssdk_eut2.h"
#include "eurotrucks2/scssdk_telemetry_eut2.h"

namespace {

const GameProfile* g_profile = nullptr;


using GenerateFn = void(__fastcall*)(std::uintptr_t company, std::uintptr_t economy);
using RefillFn = void(__fastcall*)(std::uintptr_t company, std::uintptr_t economy);

scs_log_t g_log = nullptr;
HMODULE g_plugin_module = nullptr;
std::filesystem::path g_plugin_dir;
std::uintptr_t g_module_base = 0;
bool g_driving = false;
bool g_enabled = false;
bool g_hotkey_was_down = false;

GenerationLimit g_limit;
std::uint32_t g_auto_expiry = 0;
bool g_manual_call = false;
std::uint32_t g_manual_seed = 0;
std::uintptr_t g_last_economy = 0;
void* g_clear_stub = nullptr;
bool g_patched = false;
std::array<unsigned char, 5> g_original_call{};
struct GameVector { std::uintptr_t vtable; std::uintptr_t* data; std::uint64_t count; std::uint64_t capacity; };
static_assert(sizeof(GameVector) == 32);
using ClearFn = void(__fastcall*)(GameVector*);
std::unordered_map<std::uintptr_t, std::uintptr_t> g_offer_trailers;


void Log(const char* text)
{
    if (g_log) { const std::string line = std::string("[QS - Cargo Control] ") + text; g_log(SCS_LOG_TYPE_message, line.c_str()); }
}

std::wstring ModulePath(HMODULE module)
{
    std::wstring path(32768, L'\0');
    const DWORD n = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
    if (!n || n >= path.size()) return {};
    path.resize(n);
    return path;
}

bool Sha256File(const std::wstring& path, std::wstring& result)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;

    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD object_size = 0, returned = 0, hash_size = 0;
    std::vector<UCHAR> object;
    std::array<UCHAR, 32> digest{};
    bool ok = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0;
    if (ok) ok = BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&object_size), sizeof(object_size), &returned, 0) >= 0;
    if (ok) ok = BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&hash_size), sizeof(hash_size), &returned, 0) >= 0 && hash_size == digest.size();
    if (ok) {
        object.resize(object_size);
        ok = BCryptCreateHash(algorithm, &hash, object.data(), object_size, nullptr, 0, 0) >= 0;
    }
    std::array<UCHAR, 1 << 16> buffer{};
    while (ok) {
        DWORD read = 0;
        if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr)) { ok = false; break; }
        if (!read) break;
        ok = BCryptHashData(hash, buffer.data(), read, 0) >= 0;
    }
    if (ok) ok = BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) >= 0;
    if (hash) BCryptDestroyHash(hash);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    CloseHandle(file);
    if (!ok) return false;

    constexpr wchar_t hex[] = L"0123456789ABCDEF";
    result.clear();
    result.reserve(digest.size() * 2);
    for (UCHAR b : digest) { result.push_back(hex[b >> 4]); result.push_back(hex[b & 15]); }
    return true;
}

bool WriteCode(void* address, const void* bytes, std::size_t length)
{
    DWORD previous = 0;
    if (!VirtualProtect(address, length, PAGE_EXECUTE_READWRITE, &previous)) return false;
    std::memcpy(address, bytes, length);
    FlushInstructionCache(GetCurrentProcess(), address, length);
    DWORD ignored = 0;
    VirtualProtect(address, length, previous, &ignored);
    return true;
}

std::uintptr_t TrailerKey(std::uintptr_t economy)
{
    const auto player = *reinterpret_cast<std::uintptr_t*>(economy + 0x18);
    if (!player) return 0;
    const auto reference = *reinterpret_cast<std::uintptr_t*>(player + 0x78);
    if (!reference) return 0;
    const auto owner = *reinterpret_cast<std::uintptr_t*>(reference);
    return owner ? *reinterpret_cast<std::uintptr_t*>(owner + 0x48) : 0;
}

void __fastcall KeepLiveOffers(GameVector* offers)
{
    const auto economy = *reinterpret_cast<std::uintptr_t*>(g_module_base + g_profile->economy);
    if (!economy || offers->count > 100000) return;
    const auto key = TrailerKey(economy);
    const auto vector_key = reinterpret_cast<std::uintptr_t>(offers);
    const auto known = g_offer_trailers.find(vector_key);
    if (known != g_offer_trailers.end() && known->second != key) {
        // A changed trailer requires the game's normal invalidation of offers
        // for the old trailer; preservation applies within one trailer context.
        reinterpret_cast<ClearFn>(g_module_base + g_profile->clear_vector)(offers);
    }
    g_offer_trailers[vector_key] = key;
    const auto now = *reinterpret_cast<std::uint32_t*>(economy + 0x19c);
    if (g_auto_expiry <= now) g_auto_expiry = now + 48u * 60u;
    const auto company = reinterpret_cast<std::uintptr_t>(offers) - 0xA0;
    auto* timers = *reinterpret_cast<std::uint32_t**>(company + 0x48);
    const auto timer_count = *reinterpret_cast<std::uint64_t*>(company + 0x50);
    if (timers && timer_count <= 10000)
        for (std::uint64_t n = 0; n < timer_count; ++n) timers[n] = g_auto_expiry;
    if (g_manual_call && timers && timer_count) timers[0] = g_manual_seed;
    std::vector<std::uintptr_t> expired;
    std::uint64_t retained = 0;
    for (std::uint64_t i = 0; i < offers->count; ++i) {
        const auto job = offers->data[i];
        if (job && *reinterpret_cast<std::uint32_t*>(job + 0x48) > now)
            offers->data[retained++] = job;
        else if (job) expired.push_back(job);
    }
    for (std::uint64_t i = retained; i < offers->count; ++i) offers->data[i] = 0;
    offers->count = retained;
    // Use the game's own destructor/refcount path; the temporary container owns
    // no allocation in the game and the helper only clears its element references.
    if (!expired.empty()) {
        GameVector removed{offers->vtable, expired.data(), expired.size(), expired.size()};
        reinterpret_cast<ClearFn>(g_module_base + g_profile->clear_vector)(&removed);
    }
}

bool PatchGeneratorBudget()
{
    auto* branch = reinterpret_cast<unsigned char*>(g_module_base + g_profile->attempt_branch);
    auto* immediate = reinterpret_cast<unsigned char*>(g_module_base + g_profile->attempt_immediate);
    auto* call = reinterpret_cast<unsigned char*>(g_module_base + g_profile->clear_call);
    const std::int32_t original_relative = static_cast<std::int32_t>((g_module_base + g_profile->clear_vector) - (g_module_base + (g_profile->clear_call + 5)));
    if (branch[0] != 0x73 || branch[1] != 0x0B ||
        *reinterpret_cast<std::uint32_t*>(immediate) != 4 || call[0] != 0xE8 ||
        std::memcmp(call + 1, &original_relative, 4) != 0) return false;
    for (std::uintptr_t offset = 0x4000000; offset < 0x70000000 && !g_clear_stub; offset += 0x10000)
        g_clear_stub = VirtualAlloc(reinterpret_cast<void*>(g_module_base + offset), 4096,
                                    MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!g_clear_stub) return false;
    unsigned char jump[14] = {0xFF, 0x25, 0, 0, 0, 0};
    const auto bridge = reinterpret_cast<std::uintptr_t>(&KeepLiveOffers);
    std::memcpy(jump + 6, &bridge, 8);
    std::memcpy(g_clear_stub, jump, sizeof(jump));
    DWORD old = 0;
    if (!VirtualProtect(g_clear_stub, 4096, PAGE_EXECUTE_READ, &old)) {
        VirtualFree(g_clear_stub, 0, MEM_RELEASE); g_clear_stub = nullptr; return false;
    }
    FlushInstructionCache(GetCurrentProcess(), g_clear_stub, sizeof(jump));
    std::memcpy(g_original_call.data(), call, 5);
    unsigned char replacement[5] = {0xE8};
    const auto relative = static_cast<std::int32_t>(reinterpret_cast<std::uintptr_t>(g_clear_stub) - reinterpret_cast<std::uintptr_t>(call + 5));
    std::memcpy(replacement + 1, &relative, 4);
    const unsigned char nops[2] = {0x90, 0x90};
    const std::uint32_t one = 1;
    // Roll back all sites if any write fails.
    if (!WriteCode(call, replacement, 5) || !WriteCode(branch, nops, 2) || !WriteCode(immediate, &one, 4)) {
        const unsigned char original_branch[2] = {0x73, 0x0B};
        const std::uint32_t four = 4;
        WriteCode(call, g_original_call.data(), 5);
        WriteCode(branch, original_branch, 2); WriteCode(immediate, &four, 4);
        VirtualFree(g_clear_stub, 0, MEM_RELEASE); g_clear_stub = nullptr;
        return false;
    }
    g_patched = true;
    return true;
}

void RestoreCode()
{
    if (!g_patched) return;
    const unsigned char branch[2] = {0x73, 0x0B};
    const std::uint32_t four = 4;
    WriteCode(reinterpret_cast<void*>(g_module_base + g_profile->clear_call), g_original_call.data(), 5);
    WriteCode(reinterpret_cast<void*>(g_module_base + g_profile->attempt_branch), branch, 2);
    WriteCode(reinterpret_cast<void*>(g_module_base + g_profile->attempt_immediate), &four, 4);
    VirtualFree(g_clear_stub, 0, MEM_RELEASE); g_clear_stub = nullptr; g_patched = false;
}
bool InitializeProfile()
{
    const auto exe = ModulePath(nullptr);
    std::wstring actual_hash;
    if (exe.empty() || !Sha256File(exe, actual_hash)) {
        Log("Could not hash game executable; profile disabled.");
        return false;
    }
    g_profile = FindGameProfile(actual_hash);
    if (!g_profile) {
        Log("unsupported game executable; profile disabled.");
        std::string hash;
        for (const auto digit : actual_hash) hash.push_back(static_cast<char>(digit));
        Log(("Executable SHA-256: " + hash).c_str());
        return false;
    }

    g_module_base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    if (!g_module_base) return false;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(g_module_base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(g_module_base + dos->e_lfanew);
    if (!ValidateGameProfile(*g_profile, reinterpret_cast<const unsigned char*>(g_module_base),
                             nt->OptionalHeader.SizeOfImage)) {
        Log("game code signature mismatch; profile disabled before patching.");
        return false;
    }
    if (!PatchGeneratorBudget()) {
        Log("generator patch failed; profile disabled.");
        return false;
    }
    const auto dll = ModulePath(g_plugin_module);
    if (dll.empty()) { RestoreCode(); return false; }
    g_plugin_dir = std::filesystem::path(dll).parent_path();
    notifications::Initialize(g_plugin_dir);
    if (!overlay_ui::Initialize(g_plugin_dir, Log)) {
        RestoreCode();
        notifications::Shutdown();
        Log("Plugin disabled because the in-game interface could not be initialized.");
        return false;
    }
    freight::Initialize(g_module_base, *g_profile, g_plugin_dir, Log);
    Log((std::string(g_profile->name) + " profile active; Cargo Control ETS2 v0.7.0 experimental: control menu Ctrl+Alt+F9; action shortcuts disabled by default.").c_str());
    return true;
}

void ResetSession()
{
    overlay_ui::CancelPending();
    g_limit.Reset();
    freight::Reset();
    Log("session limit reset (telemetry timer restart / economy change).");
}

void RunGlobalPass()
{
    if (!g_enabled || !g_driving) return;
    const auto real_now = GetTickCount64();
    const auto economy = *reinterpret_cast<std::uintptr_t*>(g_module_base + g_profile->economy);
    if (!economy) return;
    const auto game_now = *reinterpret_cast<std::uint32_t*>(economy + 0x19c);
    if (g_limit.Refresh(game_now)) Log("Generation slot released after 48 game hours.");
    if (g_limit.Blocked()) {
        const auto minutes = g_limit.NextAvailable() - game_now;
        wchar_t message[320];
        const auto format = notifications::Translate(L"Cooldown",
            L"Cargo market\nGeneration limit reached. Try again in %u h %02u min of game time.",
            L"Биржа грузов\nЛимит генерации исчерпан. Повторная генерация через %u ч %02u мин игрового времени.");
        swprintf_s(message, format.c_str(), minutes / 60, minutes % 60);
        notifications::Show(message); return;
    }    const auto array = *reinterpret_cast<std::uintptr_t*>(economy + 0x28);
    const auto count = *reinterpret_cast<std::uint64_t*>(economy + 0x30);
    char list_log[180];
    sprintf_s(list_log, "Cargo request: companies=%llu.", count); Log(list_log);
    if (!ValidCompanyList(array, count)) {
        Log("Cargo request rejected: company list is empty, unreadable or exceeds safety bound.");
        notifications::Show(notifications::Translate(L"InvalidCompanies",
            L"Company list unavailable. See game.log.txt.",
            L"Список компаний недоступен. Подробности в game.log.txt."));
        return;
    }

    const auto deadline = g_limit.Deadline(game_now);

    const auto started = GetTickCount64();
    g_auto_expiry = deadline;
    const auto generator = reinterpret_cast<GenerateFn>(g_module_base + g_profile->generator);
    const auto refill = reinterpret_cast<RefillFn>(g_module_base + g_profile->refill);
    const auto* companies = reinterpret_cast<const std::uintptr_t*>(array);
    std::uint64_t added = 0, full = 0, eligible = 0;
    notifications::Show(notifications::Translate(L"Generating", L"Cargo market: generating offers across all companies…", L"Биржа грузов: генерация заказов по всем компаниям…"));
    Log("append pass started.");
    for (std::uint64_t i = 0; i < count; ++i) {
        const auto company = companies[i];
        if (!company) continue;
        auto* offers = reinterpret_cast<GameVector*>(company + 0xA0);
        KeepLiveOffers(offers);
        const auto before = offers->count;
        refill(company, economy);
        const auto timer_count = *reinterpret_cast<std::uint64_t*>(company + 0x50);
        auto* timers = *reinterpret_cast<std::uint32_t**>(company + 0x48);
        if (!timer_count || !timers || timer_count > 10000) continue;
        ++eligible;
        for (std::uint64_t n = 0; n < timer_count; ++n) timers[n] = deadline;
        // Native generation rejects duplicate cargo/destination combinations.
        // Bound retries for companies without enough valid combinations.
        for (unsigned attempt = 0; attempt < 20 && offers->count < before + 5; ++attempt) {
            const auto old_count = offers->count;
            // The native RNG is seeded by the timer value. Reusing one expiry
            // would generate the same candidate repeatedly; vary the seed and
            // normalize the resulting offer timestamps immediately afterwards.
            g_manual_seed = deadline + 1u + static_cast<std::uint32_t>(
                (real_now + i * 2654435761ULL + attempt * 2246822519ULL + g_limit.Used() * 3266489917ULL) % 1000000);
            g_manual_call = true;
            generator(company, economy);
            g_manual_call = false;
            timers[0] = deadline;
            for (auto n = old_count; n < offers->count; ++n) {
                const auto job = offers->data[n];
                *reinterpret_cast<std::uint32_t*>(job + 0x48) = deadline;
                *reinterpret_cast<std::uint32_t*>(job + 0x60) = deadline;
            }
        }
        const auto difference = offers->count - before;
        added += difference;
        if (difference == 5) ++full;
    }
    if (added) g_limit.RecordSuccess(game_now);
    wchar_t message[512];
    if (!added) {
        notifications::Show(notifications::Translate(L"NoOffers",
            L"No orders could be added. Check your owned trailer and available cargo.\nThis generation slot was not consumed.",
            L"Не удалось добавить заказы. Проверьте свой прицеп и наличие подходящих грузов.\nЗапуск не израсходован."));
    } else {
        const auto format = notifications::Translate(L"Result",
            L"Cargo market\nOrders added: %llu. Available runs: %u of 3.",
            L"Биржа грузов\nДобавлено заказов: %llu. Доступно запусков: %u из 3.");
        swprintf_s(message, format.c_str(), added, g_limit.RemainingUses());
        std::wstring result = message;
        if (full < eligible) result += notifications::Translate(L"Partial",
            L"\nSome companies do not have enough suitable cargo.",
            L"\nНе во всех компаниях достаточно подходящих грузов.");
        notifications::Show(result);
    }    char log[256];
    sprintf_s(log, "append pass finished: added=%llu, five_offers=%llu/%llu, remaining=%u, elapsed_ms=%llu.",
        added, full, eligible, g_limit.RemainingUses(), GetTickCount64() - started);
    Log(log);
}

void OnFrameStart(const scs_event_t, const void* info, const scs_context_t)
{
    const auto* frame = static_cast<const scs_telemetry_frame_start_t*>(info);
    const auto economy = g_enabled ? *reinterpret_cast<std::uintptr_t*>(g_module_base + g_profile->economy) : 0;
    const bool restarted = frame && (frame->flags & SCS_TELEMETRY_FRAME_START_FLAG_timer_restart);
    if (restarted ||
        (g_last_economy && economy != g_last_economy)) ResetSession();
    if (economy && (restarted || economy != g_last_economy || g_offer_trailers.empty())) {
        g_offer_trailers.clear();
        const auto key = TrailerKey(economy);
        const auto array = *reinterpret_cast<std::uintptr_t*>(economy + 0x28);
        const auto count = *reinterpret_cast<std::uint64_t*>(economy + 0x30);
        if (key && ValidCompanyList(array, count)) {
            const auto* companies = reinterpret_cast<const std::uintptr_t*>(array);
            for (std::uint64_t i = 0; i < count; ++i)
                if (companies[i]) g_offer_trailers[companies[i] + 0xA0] = key;
        }
    }
    g_last_economy = economy;
    notifications::Tick();
    const int action = overlay_ui::Tick(g_driving);
    if (action == 1) RunGlobalPass();
    else if (action == 2 && g_enabled) freight::Request(g_driving);

}
void OnPaused(const scs_event_t, const void*, const scs_context_t) { g_driving = false; }
void OnStarted(const scs_event_t, const void*, const scs_context_t) { g_driving = true; freight::PrepareForDriving(); }
} // namespace

SCSAPI_RESULT scs_telemetry_init(const scs_u32_t version, const scs_telemetry_init_params_t* const params)
{
    if (version != SCS_TELEMETRY_VERSION_1_01) return SCS_RESULT_unsupported;
    const auto* p = static_cast<const scs_telemetry_init_params_v101_t*>(params);
    if (std::string(p->common.game_id) != SCS_GAME_ID_EUT2) return SCS_RESULT_unsupported;
    g_log = p->common.log;
    const auto* events = p->register_for_event;
    if (!events) return SCS_RESULT_generic_error;
    if (events(SCS_TELEMETRY_EVENT_frame_start, OnFrameStart, nullptr) != SCS_RESULT_ok ||
        events(SCS_TELEMETRY_EVENT_paused, OnPaused, nullptr) != SCS_RESULT_ok ||
        events(SCS_TELEMETRY_EVENT_started, OnStarted, nullptr) != SCS_RESULT_ok) {
        return SCS_RESULT_generic_error;
    }
    g_enabled = InitializeProfile();
    return SCS_RESULT_ok;
}

SCSAPI_VOID scs_telemetry_shutdown(void)
{
    overlay_ui::Shutdown();
    freight::Shutdown();
    RestoreCode();
    notifications::Shutdown();
    g_limit.Reset(); g_auto_expiry = 0;
    g_last_economy = 0; g_hotkey_was_down = false;
    g_offer_trailers.clear();
    g_enabled = false;
    g_driving = false;
    g_log = nullptr;
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        g_plugin_module = module;
        DisableThreadLibraryCalls(module);
    }
    return TRUE;
}










