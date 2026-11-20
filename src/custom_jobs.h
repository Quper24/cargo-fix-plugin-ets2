#pragma once
#include "custom_job_catalog.h"
#include "game_profile.h"
#include "custom_job_creation.h"
#include <filesystem>
#include <memory>
#include <string_view>

namespace custom_jobs {
enum class Availability { UnsupportedCode, Ready };
struct View {
    std::uint64_t session = 0;
    bool supported = false;
    bool scanned = false;
    bool names_supported = false;
    bool creation_supported = false;
    bool has_creation_result = false;
    CreateResult creation_result;
    Availability availability = Availability::UnsupportedCode;
    Catalog catalog;
    TrailerCargos trailer_cargos;
    DestinationOptions destinations;
    std::string error;
};
using Logger = void(*)(const char*);
void Initialize(std::uintptr_t module, std::size_t image_size, Logger logger);
void Reset();
void Observe(std::uintptr_t economy, bool ready);
void Scan(std::uintptr_t economy);
void RefreshTrailerCargos();
void QueueDestinations(const DestinationQuery& query);
void EnableCreation(const GameProfile& core,const CreationCode& code,creation::Backend backend);
bool QueueCreation(const JobRequest& request);
void CancelCreation();
CreateResult ExecuteCreation(std::uintptr_t economy);
std::string CreationMessage(CreateResult result,bool russian);
std::shared_ptr<const View> Snapshot();
void Shutdown();
bool ContainsName(std::string_view text, std::string_view query);
#ifdef CARGO_CONTROL_CATALOG_PREVIEW
// Offline renderer fixture only; this entry point is absent from the DLL.
void PreviewCatalog(Catalog catalog);
#endif
}
