#pragma once
#include <Windows.h>
#include <string>
namespace branding {
constexpr int PanelWidth = 800;
bool Initialize();
int Measure(HDC dc, const std::wstring& message);
void Paint(HDC dc, const RECT& bounds, const std::wstring& message);
void Shutdown();
}
