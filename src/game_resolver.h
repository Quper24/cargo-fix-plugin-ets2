#pragma once
#include "game_profile.h"
#include <string>

enum class GameKind { Unknown, ETS2, ATS };
GameKind IdentifyGame(std::string_view sdk_id);
struct ExecutableVersion { unsigned major = 0, minor = 0, patch = 0, build = 0; };
bool ReadExecutableVersion(const wchar_t* path, ExecutableVersion& out);

// A mapped PE image, not raw file bytes. Never writes to the image.
// Uses known code layouts with unique instruction/flow matches and linked references.
// The file version and hash are deliberately not inputs to compatibility selection.
bool ResolveGameProfile(const unsigned char* image, std::size_t size, GameKind game,
                        GameProfile& out, std::string& reason);
// Read-only session catalog: resolve the token pools by checked code/flow,
// independently of executable hashes and patch/build version numbers.
bool ResolveCatalogPools(const unsigned char* image, std::size_t size,
                         std::uint32_t& pools_rva, std::string& reason);
struct CatalogNameCode {
    std::uint32_t localize = 0, buffer_vtable = 0, city_location = 0, city_localized_flag = 0;
};
bool ResolveCatalogNames(const unsigned char* image, std::size_t size,
                         CatalogNameCode& out, std::string& reason);
struct CreationCode {
    std::uint32_t owned_candidates=0, base_candidates=0, destinations=0, base_slot=0;
    std::uint32_t base_factory=0, schedule=0, free_event_node=0, physical_update=0;
    std::uint32_t allocator_index=0, allocator_array=0, destination_prefix=0;
};
// Must run on the original code image, before existing generator/scheduler hooks.
bool ResolveCreationCode(const unsigned char* image, std::size_t size, const GameProfile& core,
                         CreationCode& out, std::string& reason);
struct RoadTripCode {
    std::uint32_t generate=0, factory=0, select_car=0, schedule=0, remove_event=0;
    std::uint32_t format_text=0, assign_link=0, assign_text=0, registry=0, point_format=0;
    std::uint32_t lookup_car=0,vehicle_registry=0;
};
bool ResolveRoadTripCode(const unsigned char* image,std::size_t size,GameKind game,
                         RoadTripCode& out,std::string& reason);
