#include "custom_jobs.h"
#include "company_list.h"
#include <Windows.h>
#include "game_resolver.h"
#include <cstring>
#include <mutex>

namespace custom_jobs {
namespace {
std::mutex mutex;
std::shared_ptr<const View> view = std::make_shared<View>();
std::uintptr_t module = 0, pools = 0, last_economy = 0;
std::uint64_t session = 0;
bool was_ready = false;
Logger log = nullptr;
Availability availability = Availability::UnsupportedCode;
bool Memory(std::uintptr_t address, void* bytes, std::size_t length) {
    if (!ReadableMemory(address, length)) return false;
    std::memcpy(bytes, reinterpret_cast<const void*>(address), length); return true;
}
void Publish(std::shared_ptr<View> next) {
    std::lock_guard<std::mutex> lock(mutex); view = std::move(next);
}
}
void Initialize(std::uintptr_t base, std::size_t image_size, Logger logger) {
    module = base; pools = 0; last_economy = 0; log = logger;
    availability = Availability::UnsupportedCode;
    std::uint32_t rva = 0;
    std::string reason;
    if (ResolveCatalogPools(reinterpret_cast<const unsigned char*>(module), image_size, rva, reason)) {
        pools = module + rva; availability = Availability::Ready;
        if (log) log("Session catalog code/flow checks passed; executable hash and patch/build version do not gate compatibility.");
    }
    if (!pools && log) {
        const auto message = "Experimental catalog unavailable: " + reason;
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
    else next->error = "The session catalog code is incompatible or ambiguous.";
    if (log) {
        const auto message = "Session catalog scan: companies=" + std::to_string(next->catalog.companies.size()) +
            ", cargos=" + std::to_string(next->catalog.cargos.size()) +
            ", skipped=" + std::to_string(next->catalog.skipped) + ". " + next->error;
        log(message.c_str());
    }
    Publish(std::move(next));
}
std::shared_ptr<const View> Snapshot() { std::lock_guard<std::mutex> lock(mutex); return view; }
void Shutdown() { pools = module = 0; log = nullptr; availability = Availability::UnsupportedCode; Reset(); }
#ifdef CARGO_CONTROL_CATALOG_PREVIEW
void PreviewCatalog(Catalog catalog) {
    auto next = std::make_shared<View>(); next->session = ++session;
    next->supported = next->scanned = true; next->catalog = std::move(catalog);
    next->availability = Availability::Ready;
    Publish(std::move(next));
}
#endif
}
