#pragma once
#include "game_resolver.h"
#include <filesystem>
namespace delivery_time {
using Logger=void(*)(const char*);
bool Initialize(std::uintptr_t module,std::size_t size,const CreationCode& creation,const std::filesystem::path& directory,Logger logger);
bool Available();
// Called only for a successfully created truck offer on the game frame.
void Mark(std::uintptr_t economy,std::uintptr_t company,bool owned,std::uint32_t slot,std::uintptr_t offer,bool enabled);
void Observe(std::uintptr_t economy,bool ready);
void Shutdown();
}
