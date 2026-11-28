#pragma once
#include <Windows.h>
#include <string>
#include <vector>
namespace branding {
constexpr int PanelWidth = 800;
bool Initialize();
int Measure(HDC dc, const std::wstring& message);
void Paint(HDC dc, const RECT& bounds, const std::wstring& message);
void PaintLogo(HDC dc, const RECT& bounds);
bool LogoPixels(unsigned width,unsigned height,std::vector<unsigned char>& rgba);
void Shutdown();
}
