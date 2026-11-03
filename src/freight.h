#pragma once
#include <cstdint>
#include <filesystem>
#include "game_profile.h"
namespace freight {
using Logger = void(*)(const char*);
bool Initialize(std::uintptr_t module, const GameProfile& profile, const std::filesystem::path& directory, Logger logger);
void Reset();
void PrepareForDriving();
void Request(std::uintptr_t economy);
void Shutdown();
}
