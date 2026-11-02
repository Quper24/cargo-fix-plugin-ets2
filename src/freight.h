#pragma once
#include <cstdint>
#include <filesystem>
namespace freight {
using Logger = void(*)(const char*);
bool Initialize(std::uintptr_t module, const std::filesystem::path& directory, Logger logger);
void Reset();
void PrepareForDriving();
void Request(bool driving);
void Shutdown();
}
