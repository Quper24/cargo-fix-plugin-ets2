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
