#pragma once
#include "custom_job_catalog.h"
#include "game_profile.h"
#include <filesystem>
#include <memory>

namespace custom_jobs {
enum class Availability { UnsupportedCode, Ready };
struct View {
    std::uint64_t session = 0;
    bool supported = false;
    bool scanned = false;
    Availability availability = Availability::UnsupportedCode;
    Catalog catalog;
    std::string error;
};
using Logger = void(*)(const char*);
void Initialize(std::uintptr_t module, std::size_t image_size, Logger logger);
void Reset();
void Observe(std::uintptr_t economy, bool ready);
void Scan(std::uintptr_t economy);
std::shared_ptr<const View> Snapshot();
void Shutdown();
#ifdef CARGO_CONTROL_CATALOG_PREVIEW
// Offline renderer fixture only; this entry point is absent from the DLL.
void PreviewCatalog(Catalog catalog);
#endif
}
