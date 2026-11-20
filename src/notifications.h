#pragma once
#include <filesystem>
#include <string>
namespace notifications {
void Initialize(const std::filesystem::path& plugin_dir, bool american_truck = false);
void Show(const std::wstring& message);
void Tick();
void Shutdown();
bool Russian();
std::wstring Translate(const wchar_t* key, const wchar_t* english, const wchar_t* russian);
}
