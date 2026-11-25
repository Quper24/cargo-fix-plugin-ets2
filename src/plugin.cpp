#include <Windows.h>
#include <MinHook.h>

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
#include "profile_readiness.h"
#include "game_profile.h"
#include "game_resolver.h"
#include "owned_offers.h"
#include "frame_batch.h"
#include "plugin_version.h"
#ifdef CARGO_CONTROL_ROAD_TRIP
#include "road_trip.h"
#include "coaches.h"
#endif
#ifdef CARGO_CONTROL_CUSTOM_JOBS
#include "custom_jobs.h"
#include "delivery_time.h"
#endif

#include "scssdk_telemetry.h"
#include "scssdk_telemetry_event.h"
#include "eurotrucks2/scssdk_eut2.h"
#include "eurotrucks2/scssdk_telemetry_eut2.h"
#include "amtrucks/scssdk_ats.h"

namespace {

const GameProfile* g_profile = nullptr;
GameProfile g_resolved_profile{};
GameKind g_game = GameKind::Unknown;
#ifdef CARGO_CONTROL_CUSTOM_JOBS
CreationCode g_creation_code{};
#endif


using GenerateFn = void(__fastcall*)(std::uintptr_t company, std::uintptr_t economy);
using RefillFn = void(__fastcall*)(std::uintptr_t company, std::uintptr_t economy);
GenerateFn g_original_generator = nullptr;

scs_log_t g_log = nullptr;
HMODULE g_plugin_module = nullptr;
std::filesystem::path g_plugin_dir;
std::uintptr_t g_module_base = 0;
bool g_driving = false;
bool g_enabled = false;
bool g_hotkey_was_down = false;

GenerationLimit g_limit;
ProfileReadiness g_readiness;
std::uint32_t g_auto_expiry = 0;
bool g_manual_call = false;
std::uint32_t g_manual_seed = 0;
std::uint32_t g_generator_attempts = 1;
std::uintptr_t g_last_economy = 0;
void* g_clear_stub = nullptr;
RUNTIME_FUNCTION g_budget_function{};
bool g_budget_unwind_registered = false;
bool g_patched = false;
std::array<unsigned char, 5> g_original_call{};
std::array<unsigned char, 9> g_original_budget{};
using GameVector = GameOfferVector;
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
    if (!ReadableMemory(player, 0x80)) return 0;
    const auto reference = *reinterpret_cast<std::uintptr_t*>(player + 0x78);
    if (!ReadableMemory(reference, sizeof(std::uintptr_t))) return 0;
    const auto owner = *reinterpret_cast<std::uintptr_t*>(reference);
    return ReadableMemory(owner, 0x50) ? *reinterpret_cast<std::uintptr_t*>(owner + 0x48) : 0;
}

void ReleaseOffers(GameVector& offers)
{
    reinterpret_cast<ClearFn>(g_module_base + g_profile->clear_vector)(&offers);
}

void MatchTrailer(GameVector& offers, std::uintptr_t economy)
{
    const auto key = TrailerKey(economy);
    const auto vector_key = reinterpret_cast<std::uintptr_t>(&offers);
    const auto known = g_offer_trailers.find(vector_key);
    if (known != g_offer_trailers.end() && known->second != key) {
        // A changed trailer requires the game's normal invalidation of offers
        // for the old trailer; preservation applies within one trailer context.
        ReleaseOffers(offers);
    }
    g_offer_trailers[vector_key] = key;
}
#ifdef CARGO_CONTROL_CUSTOM_JOBS
void PrepareSpecificOwned(std::uintptr_t company,std::uintptr_t economy) {
    auto& offers=*reinterpret_cast<GameVector*>(company+0xa0);
    if(ValidOfferVector(offers))MatchTrailer(offers,economy);
}
void GenerateSpecificOwned(std::uintptr_t company,std::uintptr_t economy) {
    struct RestoreValue {std::uint32_t& field;std::uint32_t previous;~RestoreValue(){field=previous;}};
    RestoreValue expiry{g_auto_expiry,g_auto_expiry},seed{g_manual_seed,g_manual_seed};
    const auto now=*reinterpret_cast<const std::uint32_t*>(economy+0x19c);
    g_auto_expiry=now+48u*60u;g_manual_seed=static_cast<std::uint32_t>(GetTickCount64());
    ManualGenerationScope manual(g_manual_call,g_generator_attempts,1);
    g_original_generator(company,economy);
}
#endif

void __fastcall KeepLiveOffers(GameVector* offers)
{
    const auto economy = *reinterpret_cast<std::uintptr_t*>(g_module_base + g_profile->economy);
    if (!economy || !ValidOfferVector(*offers)) return;
    MatchTrailer(*offers, economy);
    const auto now = *reinterpret_cast<std::uint32_t*>(economy + 0x19c);
    if (g_auto_expiry <= now) g_auto_expiry = now + 48u * 60u;
    const auto company = reinterpret_cast<std::uintptr_t>(offers) - 0xA0;
    auto* timers = *reinterpret_cast<std::uint32_t**>(company + 0x48);
    const auto timer_count = *reinterpret_cast<std::uint64_t*>(company + 0x50);
    if (timers && timer_count <= 10000)
        for (std::uint64_t n = 0; n < timer_count; ++n)
            timers[n] = g_manual_call ? ManualTimerSeed(g_auto_expiry, g_manual_seed, static_cast<std::uint32_t>(n)) : g_auto_expiry;
    RemoveExpiredOffers(*offers, now, ReleaseOffers);
}

void __fastcall GenerateAutomaticCargo(std::uintptr_t company, std::uintptr_t economy)
{
    if (!company || !economy) return;
    auto& offers = *reinterpret_cast<GameVector*>(company + 0xA0);
    if (!ValidOfferVector(offers)) return;
    if (offers.count) MatchTrailer(offers, economy);
    const auto now = *reinterpret_cast<const std::uint32_t*>(economy + 0x19c);
    RefillEmptyOwnedOffers(offers, now, [&] {
        g_original_generator(company, economy);
    }, ReleaseOffers);
}

bool InstallAutomaticCargoHook()
{
    auto* target = reinterpret_cast<void*>(g_module_base + g_profile->generator);
    if (MH_CreateHook(target, reinterpret_cast<void*>(&GenerateAutomaticCargo),
                      reinterpret_cast<void**>(&g_original_generator)) != MH_OK) return false;
    if (MH_EnableHook(target) != MH_OK) {
        MH_RemoveHook(target);
        g_original_generator = nullptr;
        return false;
    }
    return true;
}

void RemoveAutomaticCargoHook()
{
    if (!g_original_generator) return;
    MH_RemoveHook(reinterpret_cast<void*>(g_module_base + g_profile->generator));
    g_original_generator = nullptr;
}

bool PatchGeneratorBudget()
{
    auto* branch = reinterpret_cast<unsigned char*>(g_module_base + g_profile->attempt_branch);
    auto* immediate = reinterpret_cast<unsigned char*>(g_module_base + g_profile->attempt_immediate);
    auto* call = reinterpret_cast<unsigned char*>(g_module_base + g_profile->clear_call);
    auto* budget_instruction = reinterpret_cast<unsigned char*>(g_module_base + g_profile->attempt_branch + 2);
    const std::int32_t original_relative = static_cast<std::int32_t>((g_module_base + g_profile->clear_vector) - (g_module_base + (g_profile->clear_call + 5)));
    if (branch[0] != 0x73 || branch[1] != 0x0B ||
        *reinterpret_cast<std::uint32_t*>(immediate) != 4 || call[0] != 0xE8 ||
        std::memcmp(call + 1, &original_relative, 4) != 0 ||
        g_profile->attempt_immediate != g_profile->attempt_branch + 7 ||
        std::memcmp(budget_instruction, "\x48\xc7\x44\x24", 4) ||
        (budget_instruction[4] != 0x70 && budget_instruction[4] != 0x78)) return false;
    for (std::uintptr_t offset = 0x4000000; offset < 0x70000000 && !g_clear_stub; offset += 0x10000)
        g_clear_stub = VirtualAlloc(reinterpret_cast<void*>(g_module_base + offset), 4096,
                                    MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!g_clear_stub) return false;
    unsigned char jump[14] = {0xFF, 0x25, 0, 0, 0, 0};
    const auto bridge = reinterpret_cast<std::uintptr_t>(&KeepLiveOffers);
    std::memcpy(jump + 6, &bridge, 8);
    std::memcpy(g_clear_stub, jump, sizeof(jump));
    auto* budget_stub = static_cast<unsigned char*>(g_clear_stub) + 32;
    const auto stub_code = MakeAttemptBudgetStub(reinterpret_cast<std::uintptr_t>(&g_generator_attempts), budget_instruction[4]);
    std::memcpy(budget_stub, stub_code.data(), stub_code.size());
    std::memcpy(static_cast<unsigned char*>(g_clear_stub)+64, AttemptBudgetUnwindInfo.data(), AttemptBudgetUnwindInfo.size());
    DWORD old = 0;
    if (!VirtualProtect(g_clear_stub, 4096, PAGE_EXECUTE_READ, &old)) {
        VirtualFree(g_clear_stub, 0, MEM_RELEASE); g_clear_stub = nullptr; return false;
    }
    FlushInstructionCache(GetCurrentProcess(), g_clear_stub, 32 + stub_code.size());
    g_budget_function = {32, static_cast<DWORD>(32+stub_code.size()), 64};
    if (!RtlAddFunctionTable(&g_budget_function, 1, reinterpret_cast<DWORD64>(g_clear_stub))) {
        VirtualFree(g_clear_stub, 0, MEM_RELEASE); g_clear_stub = nullptr; return false;
    }
    g_budget_unwind_registered = true;
    std::memcpy(g_original_call.data(), call, 5);
    std::memcpy(g_original_budget.data(), budget_instruction, g_original_budget.size());
    unsigned char replacement[5] = {0xE8};
    const auto relative = static_cast<std::int32_t>(reinterpret_cast<std::uintptr_t>(g_clear_stub) - reinterpret_cast<std::uintptr_t>(call + 5));
    std::memcpy(replacement + 1, &relative, 4);
    const unsigned char nops[2] = {0x90, 0x90};
    std::array<unsigned char,9> budget_call{}; budget_call.fill(0x90); budget_call[0] = 0xe8;
    const auto budget_relative = static_cast<std::int32_t>(reinterpret_cast<std::uintptr_t>(budget_stub) - reinterpret_cast<std::uintptr_t>(budget_instruction + 5));
    std::memcpy(budget_call.data()+1, &budget_relative, 4);
    g_generator_attempts = 1;
    // Roll back all sites if any write fails.
    if (!WriteCode(call, replacement, 5) || !WriteCode(branch, nops, 2) ||
        !WriteCode(budget_instruction, budget_call.data(), budget_call.size())) {
        const unsigned char original_branch[2] = {0x73, 0x0B};
        WriteCode(call, g_original_call.data(), 5);
        WriteCode(branch, original_branch, 2);
        WriteCode(budget_instruction, g_original_budget.data(), g_original_budget.size());
        RtlDeleteFunctionTable(&g_budget_function); g_budget_unwind_registered = false;
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
    WriteCode(reinterpret_cast<void*>(g_module_base + g_profile->clear_call), g_original_call.data(), 5);
    WriteCode(reinterpret_cast<void*>(g_module_base + g_profile->attempt_branch), branch, 2);
    WriteCode(reinterpret_cast<void*>(g_module_base + g_profile->attempt_branch + 2), g_original_budget.data(), g_original_budget.size());
    g_generator_attempts = 1;
    if (g_budget_unwind_registered) { RtlDeleteFunctionTable(&g_budget_function); g_budget_unwind_registered = false; }
    VirtualFree(g_clear_stub, 0, MEM_RELEASE); g_clear_stub = nullptr; g_patched = false;
}
bool InitializeProfile()
{
    const auto exe = ModulePath(nullptr);
    g_profile = nullptr;
    if (exe.empty()) return false;
    g_module_base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    if (!g_module_base) return false;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(g_module_base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(g_module_base + dos->e_lfanew);
    ExecutableVersion version{};
    const bool version_read = ReadExecutableVersion(exe.c_str(), version);
    if (version_read) {
        char details[160];
        sprintf_s(details, "%s executable version %u.%u.%u.%u.", g_game == GameKind::ATS ? "ATS" : "ETS2",
                  version.major, version.minor, version.patch, version.build);
        Log(details);
    }
    std::string reason;
    if (!ResolveGameProfile(reinterpret_cast<const unsigned char*>(g_module_base),
                            nt->OptionalHeader.SizeOfImage, g_game, g_resolved_profile, reason)) {
        Log(("Compatibility check failed before patching: " + reason).c_str());
        return false;
    }
    g_profile = &g_resolved_profile;
    Log("Compatible instruction/flow and reference checks passed; executable version/hash do not gate compatibility.");
    if (!ValidateGameProfile(*g_profile, reinterpret_cast<const unsigned char*>(g_module_base),
                             nt->OptionalHeader.SizeOfImage)) {
        Log("game code signature mismatch; profile disabled before patching.");
        return false;
    }
#ifdef CARGO_CONTROL_CUSTOM_JOBS
    g_creation_code={};
    if(!ResolveCreationCode(reinterpret_cast<const unsigned char*>(g_module_base),nt->OptionalHeader.SizeOfImage,*g_profile,g_creation_code,reason))
        Log(("Custom job creation disabled before patching: "+reason).c_str());
#endif
#ifdef CARGO_CONTROL_ROAD_TRIP
    // Road Trip shares the native event scheduler with truck offers. Resolve its
    // original code before installing any of our patches or MinHook detours.
    road_trip::Initialize(g_module_base,nt->OptionalHeader.SizeOfImage,g_game,*g_profile,g_creation_code,Log);
#endif
#ifdef CARGO_CONTROL_COACHES
    coaches::Initialize(g_module_base,nt->OptionalHeader.SizeOfImage,g_game,*g_profile,g_creation_code,Log);
#endif
    if (!PatchGeneratorBudget()) {
        Log("generator patch failed; profile disabled.");
        return false;
    }
    const auto dll = ModulePath(g_plugin_module);
    if (dll.empty()) { RestoreCode(); return false; }
    g_plugin_dir = std::filesystem::path(dll).parent_path();
    notifications::Initialize(g_plugin_dir, g_game == GameKind::ATS);
    if (!overlay_ui::Initialize(g_plugin_dir, Log)) {
        RestoreCode();
        notifications::Shutdown();
        Log("Plugin disabled because the in-game interface could not be initialized.");
        return false;
    }
    if (!InstallAutomaticCargoHook()) {
        RestoreCode();
        overlay_ui::Shutdown();
        notifications::Shutdown();
        Log("Could not install the automatic owned-cargo hook. Plugin disabled.");
        return false;
    }
    freight::Initialize(g_module_base, *g_profile, g_plugin_dir, Log);
#ifdef CARGO_CONTROL_CUSTOM_JOBS
    delivery_time::Initialize(g_module_base,nt->OptionalHeader.SizeOfImage,g_creation_code,g_plugin_dir,Log);
    custom_jobs::Initialize(g_module_base, nt->OptionalHeader.SizeOfImage, Log);
    custom_jobs::EnableCreation(*g_profile,g_creation_code,{PrepareSpecificOwned,GenerateSpecificOwned});
#endif
    Log((std::string(g_profile->name) + " profile active. Cargo Control v" CARGO_CONTROL_VERSION_TEXT
         ". Control menu shortcut is configurable; default Ctrl+Alt+F9. Action shortcuts disabled by default.").c_str());
    return true;
}

void CancelOwnedRefresh();
bool OwnedRefreshBusy();
void ResetSession()
{
    overlay_ui::CancelPending();CancelOwnedRefresh();
#ifdef CARGO_CONTROL_COACHES
    coaches::Reset();
#endif
#ifdef CARGO_CONTROL_ROAD_TRIP
    road_trip::Reset();
#endif
#ifdef CARGO_CONTROL_CUSTOM_JOBS
    custom_jobs::Reset();
#endif
    g_limit.Reset();
    g_auto_expiry = 0;
    freight::Reset();
    Log("session limit reset (telemetry timer restart / economy change).");
}

struct OwnedRefreshState {
    FrameBatch work;
    std::uintptr_t economy=0,trailer=0;
    std::uint32_t game_now=0,deadline=0;
    std::uint64_t real_now=0,started=0,generated=0,full=0,eligible=0,duplicate_offers=0,excess_offers=0,native_calls=0,replaced=0,peak_company=0,preserved=0;
    std::array<std::uint64_t,6> distribution{};
} g_owned_refresh;
void CancelOwnedRefresh(){g_owned_refresh.work.Cancel();}
bool OwnedRefreshBusy(){return g_owned_refresh.work.Active();}
void RefreshOwnedCargo(std::uintptr_t economy)
{
    if (!g_enabled) return;
    if (!TrailerKey(economy)) {
        notifications::Show(notifications::Translate(L"TrailerRequired",
            L"Select your owned trailer to refresh cargo offers. You can refresh Freight / Quick jobs without an owned truck.",
            L"Для обновления грузов выберите собственный прицеп. Обновление рынка и заказов агентств доступно без своего грузовика."));
        return;
    }
    const auto real_now = GetTickCount64();
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

    g_owned_refresh={};
    auto& state=g_owned_refresh;
    state.economy=economy;state.trailer=TrailerKey(economy);state.game_now=game_now;
    state.deadline=g_limit.Deadline(game_now);state.real_now=real_now;state.started=GetTickCount64();
    state.work.Begin(reinterpret_cast<const std::uintptr_t*>(array),static_cast<std::size_t>(count));
    overlay_ui::SetProgress(0,count);
    Log("Owned cargo replacement started in frame batches; old offers survive empty generation.");
}
void TickOwnedRefresh(std::uintptr_t economy){
    auto& state=g_owned_refresh;
    if(!state.work.Active())return;
    if(economy!=state.economy || TrailerKey(economy)!=state.trailer){
        state.work.Cancel();notifications::Show(L"Обновление грузов остановлено: сессия или прицеп изменились.");return;
    }
    const auto deadline=state.deadline,game_now=state.game_now;
    const auto real_now=state.real_now,started=state.started;
    auto& generated=state.generated;auto& full=state.full;auto& eligible=state.eligible;
    auto& duplicate_offers=state.duplicate_offers;auto& excess_offers=state.excess_offers;auto& native_calls=state.native_calls;
    auto& replaced=state.replaced;auto& peak_company=state.peak_company;auto& distribution=state.distribution;
    g_auto_expiry=deadline;
    const auto refill=reinterpret_cast<RefillFn>(g_module_base+g_profile->refill);
    state.work.Step([&](std::uintptr_t company,std::size_t i){
        if (!company) return;
#ifdef CARGO_CONTROL_CUSTOM_JOBS
        if(custom_jobs::RoadTripOnly(company))return;
#endif
        auto* offers = reinterpret_cast<GameVector*>(company + 0xA0);
        if (!ValidOfferVector(*offers)) return;
        refill(company, economy);
        const auto timer_count = *reinterpret_cast<std::uint64_t*>(company + 0x50);
        auto* timers = *reinterpret_cast<std::uint32_t**>(company + 0x48);
        if (!timer_count || timer_count > 10000 ||
            !ReadableMemory(reinterpret_cast<std::uintptr_t>(timers), timer_count * sizeof(*timers))) return;
        ++eligible;
        const bool special_first_slot = *reinterpret_cast<std::uintptr_t*>(company + 0x108) != 0;
        MatchTrailer(*offers,economy);
        const auto previous_count=offers->count;
        const std::vector<std::uint32_t> old_timers(timers,timers+timer_count);
        const auto replacement = ReplaceOwnedOffersSafely(*offers, timer_count, special_first_slot, [&](std::uint32_t budget, unsigned attempt) {
            const auto old_count = offers->count;
            // Slot zero can be a fixed special offer. Reach the later native slots
            // with separate seeds, then normalize expiry and discard new duplicates.
            g_manual_seed = static_cast<std::uint32_t>(
                (real_now + i * 2654435761ULL + attempt * 2246822519ULL + g_limit.Used() * 3266489917ULL) % 1000000);
            {
                ManualGenerationScope manual(g_manual_call, g_generator_attempts, budget);
#ifdef CARGO_CONTROL_CUSTOM_JOBS
                custom_jobs::creation::ManualMarketScope market_scope;
#endif
                g_original_generator(company, economy);
                ++native_calls;
            }
            for (std::uint64_t n = 0; n < timer_count; ++n) timers[n] = deadline;
            for (auto n = old_count; n < offers->count; ++n) {
                const auto job = offers->data[n];
                *reinterpret_cast<std::uint32_t*>(job + 0x48) = deadline;
                *reinterpret_cast<std::uint32_t*>(job + 0x60) = deadline;
            }
        },ReleaseOffers,[](GameVector& vector,std::uint64_t required){
            if(vector.capacity>=required)return true;
            using Reserve=bool(__fastcall*)(GameVector*,std::uint64_t);
            if(!ReadableMemory(vector.vtable+24,8))return false;
            const auto reserve=*reinterpret_cast<const std::uintptr_t*>(vector.vtable+24);
            return ReadableMemory(reserve,1) && reinterpret_cast<Reserve>(reserve)(&vector,required);
        });
        if(!replacement.committed){std::copy(old_timers.begin(),old_timers.end(),timers);++g_owned_refresh.preserved;++distribution[0];return;}
        replaced+=previous_count;
        duplicate_offers += replacement.cleanup.duplicates;
        excess_offers += replacement.cleanup.excess;
        generated += offers->count;
        peak_company = std::max(peak_company, offers->count);
        if (offers->count == 5) ++full;
        ++distribution[std::min<std::uint64_t>(offers->count, 5)];
    },[]{return GetTickCount64();});
    overlay_ui::SetProgress(state.work.Done(),state.work.Total());
    if(state.work.Active())return;
    Log(("Owned refresh preserved companies after empty generation: "+std::to_string(state.preserved)+".").c_str());
    if (generated) g_limit.RecordSuccess(game_now);
    wchar_t message[512];
    if (!generated) {
        notifications::Show(notifications::Translate(L"NoOffers",
            L"No new offers were generated. Check your owned trailer and available cargo.\nThis refresh slot is still available.",
            L"Новых грузов нет. Проверьте свой прицеп и доступные грузы.\nЗапуск обновления доступен."));
    } else {
        const auto format = notifications::Translate(L"Result",
            L"Cargo market refreshed\nNew offers: %llu. Available runs: %u of 3.",
            L"Биржа грузов обновлена\nНовых предложений: %llu. Доступно запусков: %u из 3.");
        swprintf_s(message, format.c_str(), generated, g_limit.RemainingUses());
        std::wstring result = message;
        wchar_t company_result[160];
        const auto company_format = notifications::Translate(L"CompanyResult",
            L"\nCompanies with five new offers: %llu of %llu.",
            L"\nКомпаний с пятью новыми предложениями: %llu из %llu.");
        swprintf_s(company_result, company_format.c_str(), full, eligible);
        result += company_result;
        if (full < eligible) result += notifications::Translate(L"Partial",
            L"\nSome companies received fewer than five new offers.",
            L"\nУ части компаний меньше пяти новых предложений.");
        result+=notifications::Translate(L"ReopenCargoMarket",L"\nReopen the cargo market to see the updated offers.",L"\nЗаново откройте биржу грузов, чтобы увидеть обновлённые предложения.");
        notifications::Show(result);
    }    char log[256];
    sprintf_s(log, "Owned cargo replacement finished. Replaced=%llu, generated=%llu, peak_company=%llu, five_offers=%llu/%llu, remaining=%u, elapsed_ms=%llu.",
        replaced, generated, peak_company, full, eligible, g_limit.RemainingUses(), GetTickCount64() - started);
    Log(log);
    char details[320];
    sprintf_s(details, "Owned cargo unique additions by company: zero=%llu, one=%llu, two=%llu, three=%llu, four=%llu, five=%llu; duplicates_removed=%llu, excess_removed=%llu, native_calls=%llu.",
        distribution[0],distribution[1],distribution[2],distribution[3],distribution[4],distribution[5],duplicate_offers,excess_offers,native_calls);
    Log(details);
}

void OnFrameStart(const scs_event_t, const void* info, const scs_context_t)
{
    const auto* frame = static_cast<const scs_telemetry_frame_start_t*>(info);
    const auto economy = g_enabled ? *reinterpret_cast<std::uintptr_t*>(g_module_base + g_profile->economy) : 0;
    EconomySnapshot snapshot{};
    const bool valid = ReadEconomySnapshot(economy, snapshot);
    const bool restarted = frame && (frame->flags & SCS_TELEMETRY_FRAME_START_FLAG_timer_restart);
    if (restarted ||
        (g_last_economy && economy != g_last_economy)) ResetSession();
    if (valid && (restarted || economy != g_last_economy || g_offer_trailers.empty())) {
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
    const bool ready = g_readiness.Observe(snapshot, valid, restarted);
#ifdef CARGO_CONTROL_COACHES
    const bool coaches_was_busy=coaches::Busy();coaches::Observe(economy,ready);
    if(coaches_was_busy&&!coaches::Busy()){overlay_ui::FinishAction();return;}
    if(coaches::Busy()){
        coaches::Tick(economy);
        if(!coaches::Busy())overlay_ui::FinishAction();return;
    }
#endif
#ifdef CARGO_CONTROL_CUSTOM_JOBS
    delivery_time::Observe(economy,ready);
#endif
#ifdef CARGO_CONTROL_ROAD_TRIP
    const bool road_was_busy=road_trip::Busy();road_trip::Observe(economy,ready);
    if(road_was_busy&&!road_trip::Busy()){overlay_ui::FinishAction();return;}
    if(road_trip::Busy()){
        road_trip::Tick(economy);
        if(!road_trip::Busy())overlay_ui::FinishAction();return;
    }
#endif
#ifdef CARGO_CONTROL_CUSTOM_JOBS
    const bool scan_was_busy=custom_jobs::ScanBusy();
    custom_jobs::Observe(economy, ready);
    if(scan_was_busy&&!custom_jobs::ScanBusy()){overlay_ui::FinishAction(true,1);return;}
    if(custom_jobs::ScanBusy()){
        custom_jobs::TickScan(economy);
        if(custom_jobs::ScanBusy())overlay_ui::SetProgress(custom_jobs::ScanDone(),custom_jobs::ScanTotal());
        else overlay_ui::FinishAction(true,1);
        return;
    }
#endif
    if(OwnedRefreshBusy() || freight::Busy()){
        if(!ready){
            CancelOwnedRefresh();freight::Cancel();overlay_ui::FinishAction();
            notifications::Show(notifications::Translate(L"RefreshInterrupted",L"Market refresh stopped because the loaded profile changed.",L"Обновление рынка остановлено: данные загруженного профиля изменились."));
            Log("Frame-batched market refresh cancelled: profile is not ready.");return;
        }
        TickOwnedRefresh(economy);freight::Tick(economy);
        if(!OwnedRefreshBusy() && !freight::Busy())overlay_ui::FinishAction();
        return;
    }
    // Consume every request even in menus: never defer it until a future drive/save.
    const int action = overlay_ui::Tick(ready);
    struct FinishAction {int action;bool reopen=false;int page=1;~FinishAction(){
#ifdef CARGO_CONTROL_CUSTOM_JOBS
        if(custom_jobs::ScanBusy())return;
#endif
#ifdef CARGO_CONTROL_COACHES
        if(coaches::Busy())return;
#endif
#ifdef CARGO_CONTROL_ROAD_TRIP
        if(road_trip::Busy())return;
#endif
        if(action && !OwnedRefreshBusy() && !freight::Busy())overlay_ui::FinishAction(reopen,page);
    }} finish{action};
    if (action && !ready) {
#ifdef CARGO_CONTROL_CUSTOM_JOBS
        custom_jobs::CancelCreation();
#endif
        notifications::Show(notifications::Translate(L"ProfileNotReady",
            L"Profile data is not ready. Wait for loading to finish, then try again from the garage.",
            L"Данные профиля ещё не готовы. Дождитесь окончания загрузки и повторите действие в гараже."));
        Log("Manual request rejected: profile/economy is not ready.");
    }
#ifdef CARGO_CONTROL_ROAD_TRIP
    else if(action==5&&g_enabled){road_trip::Scan(economy);finish.reopen=true;finish.page=2;}
    else if(action==6&&g_enabled)road_trip::Refresh(economy);
    else if(action==7&&g_enabled)road_trip::Create(economy);
#endif
#ifdef CARGO_CONTROL_COACHES
    else if(action==8&&g_enabled){coaches::Scan(economy);finish.reopen=true;finish.page=3;}
    else if(action==9&&g_enabled)coaches::Refresh(economy);
    else if(action==10&&g_enabled)coaches::Create(economy);
#endif
    else if (action == 1) RefreshOwnedCargo(economy);
    else if (action == 2 && g_enabled) freight::Request(economy);
#ifdef CARGO_CONTROL_CUSTOM_JOBS
    else if(action==3 && g_enabled){custom_jobs::Scan(economy);overlay_ui::SetProgress(custom_jobs::ScanDone(),custom_jobs::ScanTotal());finish.reopen=true;}
    else if (action == 4 && g_enabled) {
        const auto result=custom_jobs::ExecuteCreation(economy);
        const auto text=custom_jobs::CreationMessage(result,notifications::Russian());
        const int length=MultiByteToWideChar(CP_UTF8,0,text.data(),static_cast<int>(text.size()),nullptr,0);
        std::wstring message(length,L'\0');
        if(length)MultiByteToWideChar(CP_UTF8,0,text.data(),static_cast<int>(text.size()),message.data(),length);
        notifications::Show(message);
    }
#endif

}
void OnPaused(const scs_event_t, const void*, const scs_context_t) { g_driving = false; }
void OnStarted(const scs_event_t, const void*, const scs_context_t) { g_driving = true; freight::PrepareForDriving(); }
} // namespace

SCSAPI_RESULT scs_telemetry_init(const scs_u32_t version, const scs_telemetry_init_params_t* const params)
{
    if (version != SCS_TELEMETRY_VERSION_1_01) return SCS_RESULT_unsupported;
    const auto* p = static_cast<const scs_telemetry_init_params_v101_t*>(params);
    if (!p->common.game_id) return SCS_RESULT_unsupported;
    g_game = IdentifyGame(p->common.game_id);
    if (g_game == GameKind::Unknown) return SCS_RESULT_unsupported;
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
#ifdef CARGO_CONTROL_COACHES
    coaches::Reset();
#endif
#ifdef CARGO_CONTROL_ROAD_TRIP
    road_trip::Reset();
#endif
    CancelOwnedRefresh();freight::Cancel();
#ifdef CARGO_CONTROL_CUSTOM_JOBS
    delivery_time::Shutdown();
    custom_jobs::Shutdown();
#endif
    RemoveAutomaticCargoHook();
    overlay_ui::Shutdown();
    freight::Shutdown();
    RestoreCode();
    notifications::Shutdown();
    g_limit.Reset(); g_auto_expiry = 0; g_readiness = {};
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










