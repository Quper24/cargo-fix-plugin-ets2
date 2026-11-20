#include "custom_jobs.h"
#include "company_list.h"
#include <Windows.h>
#include <bcrypt.h>
#include <array>
#include <cstring>
#include <fstream>
#include <mutex>

namespace custom_jobs {
namespace {
struct Profile {
    const char* name;
    const char* sha256;
    std::uint32_t generator, fill, pools, guard_rva;
    std::array<unsigned char, 7> guard;
};
constexpr Profile profiles[] = {
#include "custom_job_profiles.inc"
};
std::mutex mutex;
std::shared_ptr<const View> view = std::make_shared<View>();
std::uintptr_t module = 0, pools = 0, last_economy = 0;
std::uint64_t session = 0;
bool was_ready = false;
Logger log = nullptr;
Availability availability = Availability::UnsupportedExe;
bool Memory(std::uintptr_t address, void* bytes, std::size_t length) {
    if (!ReadableMemory(address, length)) return false;
    std::memcpy(bytes, reinterpret_cast<const void*>(address), length); return true;
}
std::string Digest(const std::filesystem::path& executable) {
    std::ifstream input(executable, std::ios::binary);
    if (!input) return {};
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return {};
    std::array<unsigned char, 32> digest{};
    bool ok = BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) >= 0;
    std::array<char, 65536> bytes{};
    while (ok && input) {
        input.read(bytes.data(), bytes.size());
        if (input.gcount()) ok = BCryptHashData(hash, reinterpret_cast<PUCHAR>(bytes.data()),
                                               static_cast<ULONG>(input.gcount()), 0) >= 0;
    }
    ok = ok && !input.bad() && BCryptFinishHash(hash, digest.data(), digest.size(), 0) >= 0;
    if (hash) BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (!ok) return {};
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    for (auto byte : digest) { result += hex[byte >> 4]; result += hex[byte & 15]; }
    return result;
}
void Publish(std::shared_ptr<View> next) {
    std::lock_guard<std::mutex> lock(mutex); view = std::move(next);
}
}
void Initialize(std::uintptr_t base, const GameProfile& game,
                const std::filesystem::path& executable, Logger logger) {
    module = base; pools = 0; last_economy = 0; log = logger;
    const auto digest = Digest(executable);
    availability = digest.empty() ? Availability::UnreadableExe : Availability::UnsupportedExe;
    for (const auto& profile : profiles) {
        if (digest != profile.sha256) continue;
        if (game.generator != profile.generator || game.fill_company != profile.fill) {
            availability = Availability::CoreMismatch; break;
        }
        availability = Availability::CodeMismatch;
        std::array<unsigned char, 7> guard{};
        if (Memory(module + profile.guard_rva, guard.data(), guard.size()) && guard == profile.guard) {
            pools = module + profile.pools;
            availability = Availability::Ready;
            if (log) log("Experimental session catalog enabled for an exact researched EXE; catalog performs no game/save writes.");
        }
        break;
    }
    if (!pools && log) {
        constexpr const char* reasons[] = {"unsupported_exe", "unreadable_exe", "core_mismatch", "code_mismatch", "ready"};
        const auto message = std::string("Experimental catalog unavailable: reason=") + reasons[static_cast<int>(availability)] +
            ", sha256=" + (digest.empty() ? "unreadable" : digest) +
            ", generator=" + std::to_string(game.generator) + ", fill=" + std::to_string(game.fill_company) + ".";
        log(message.c_str());
    }
    Reset();
}
void Reset() {
    last_economy = 0; was_ready = false;
    auto next = std::make_shared<View>(); next->session = ++session; next->supported = pools != 0;
    next->availability = availability;
    Publish(std::move(next));
}
void Observe(std::uintptr_t economy, bool ready) {
    if (was_ready && (!ready || economy != last_economy)) Reset();
    was_ready = ready; last_economy = ready ? economy : 0;
}
void Scan(std::uintptr_t economy) {
    if (last_economy && last_economy != economy) Reset();
    last_economy = economy;
    auto next = std::make_shared<View>(); next->session = session; next->supported = pools != 0;
    next->availability = availability;
    if (pools) next->scanned = BuildCatalog(Memory, economy, pools, next->catalog, next->error);
    else next->error = "This EXE has no verified experimental catalog profile.";
    if (log) {
        const auto message = "Session catalog scan: companies=" + std::to_string(next->catalog.companies.size()) +
            ", cargos=" + std::to_string(next->catalog.cargos.size()) +
            ", skipped=" + std::to_string(next->catalog.skipped) + ". " + next->error;
        log(message.c_str());
    }
    Publish(std::move(next));
}
std::shared_ptr<const View> Snapshot() { std::lock_guard<std::mutex> lock(mutex); return view; }
void Shutdown() { pools = module = 0; log = nullptr; availability = Availability::UnsupportedExe; Reset(); }
#ifdef CARGO_CONTROL_CATALOG_PREVIEW
void PreviewCatalog(Catalog catalog) {
    auto next = std::make_shared<View>(); next->session = ++session;
    next->supported = next->scanned = true; next->catalog = std::move(catalog);
    next->availability = Availability::Ready;
    Publish(std::move(next));
}
#endif
}
