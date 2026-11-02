#pragma once
#include <Windows.h>
namespace menu_style {
constexpr int Width = 900, Height = 660;
constexpr COLORREF Background = RGB(0,0,0), Surface = RGB(19,25,21), Accent = RGB(180,221,171);
void Paint(HDC dc, bool russian);
void Hotkey(HDC dc, RECT bounds, WORD binding, bool focused, bool russian);
void Button(const DRAWITEMSTRUCT& item);
}
