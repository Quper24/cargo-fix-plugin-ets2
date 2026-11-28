#pragma once
#include <filesystem>
#include <string>
#include <cstdint>
struct IDXGISwapChain;
namespace overlay_ui {
using Logger = void(*)(const char*);
bool Initialize(const std::filesystem::path& directory, Logger logger, bool install_present_hook = true);
void RenderFrame(IDXGISwapChain* chain, bool allow_hidden = false, bool preview_menu = false, int preview_page = -1);
void Notify(const std::wstring& text);
void Request(int action);
int Tick(bool ready=true);
void CancelPending();
void FinishAction(bool show_catalog=false,int page=1);
void SetProgress(std::uint64_t done,std::uint64_t total);
void Shutdown();
int ScalePercent();
bool SetScalePercent(int percent);
bool LightTheme();
bool SetLightTheme(bool light);
struct Diagnostics { std::uint64_t frames; std::uint64_t present_calls; std::uint64_t input_calls; bool logo; bool ready; int operation; bool menu; };
Diagnostics Inspect();
}
