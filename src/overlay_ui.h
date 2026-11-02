#pragma once
#include <filesystem>
#include <string>
#include <cstdint>
struct IDXGISwapChain;
namespace overlay_ui {
using Logger = void(*)(const char*);
bool Initialize(const std::filesystem::path& directory, Logger logger, bool install_present_hook = true);
void RenderFrame(IDXGISwapChain* chain, bool allow_hidden = false, bool preview_menu = false);
void Notify(const std::wstring& text);
int Tick(bool driving);
void CancelPending();
void Shutdown();
struct Diagnostics { std::uint64_t frames; std::uint64_t present_calls; std::uint64_t input_calls; bool logo; bool ready; };
Diagnostics Inspect();
}
