#include "branding.h"
#include <objidl.h>
#include <gdiplus.h>
#include <algorithm>
#include <cstring>

namespace branding {
namespace {
ULONG_PTR token = 0;
Gdiplus::Image* logo = nullptr;
IStream* logo_stream = nullptr;
constexpr COLORREF Background = RGB(0, 0, 0);
constexpr COLORREF Accent = RGB(180, 221, 171);
constexpr COLORREF Border = RGB(58, 80, 53);
constexpr COLORREF Foreground = RGB(245, 247, 244);
constexpr int LogoWidth = 160;
constexpr int Left = 24;
constexpr int BodyTop = 50;
int BodyRight(int width) { return width - LogoWidth - 48; }
HFONT Font(int height, int weight) {
    return CreateFontW(-height, 0, 0, 0, weight, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
}
void Fill(HDC dc, const RECT& rect, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    FillRect(dc, &rect, brush); DeleteObject(brush);
}
}

bool Initialize() {
    if (logo) return true;
    Gdiplus::GdiplusStartupInput input;
    if (Gdiplus::GdiplusStartup(&token, &input, nullptr) != Gdiplus::Ok) { token = 0; return false; }
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&Initialize), &self);
    HRSRC resource = FindResourceW(self, MAKEINTRESOURCEW(101), MAKEINTRESOURCEW(10));
    if (!resource) { Shutdown(); return false; }
    const DWORD size = SizeofResource(self, resource);
    const void* source = LockResource(LoadResource(self, resource));
    if (!source || !size) { Shutdown(); return false; }
    HGLOBAL bytes = GlobalAlloc(GMEM_MOVEABLE, size);
    if (!bytes) { Shutdown(); return false; }
    void* destination = GlobalLock(bytes);
    if (!destination) { GlobalFree(bytes); Shutdown(); return false; }
    std::memcpy(destination, source, size); GlobalUnlock(bytes);
    if (FAILED(CreateStreamOnHGlobal(bytes, TRUE, &logo_stream))) {
        GlobalFree(bytes); Shutdown(); return false;
    }
    logo = Gdiplus::Image::FromStream(logo_stream, FALSE);
    if (!logo || logo->GetLastStatus() != Gdiplus::Ok || !logo->GetWidth() || !logo->GetHeight()) {
        Shutdown(); return false;
    }
    return true;
}

int Measure(HDC dc, const std::wstring& message) {
    HFONT font = Font(20, FW_NORMAL);
    const auto previous = SelectObject(dc, font);
    RECT text{0, 0, BodyRight(PanelWidth) - Left, 0};
    DrawTextW(dc, message.c_str(), -1, &text, DT_CALCRECT | DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
    SelectObject(dc, previous); DeleteObject(font);
    return std::clamp(static_cast<int>(text.bottom) + BodyTop + 24, 164, 360);
}

void Paint(HDC dc, const RECT& bounds, const std::wstring& message) {
    Fill(dc, bounds, Background);
    HBRUSH border = CreateSolidBrush(Border); FrameRect(dc, &bounds, border); DeleteObject(border);
    RECT accent{bounds.left, bounds.top, bounds.left + 4, bounds.bottom}; Fill(dc, accent, Accent);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, Accent);
    HFONT heading = Font(16, FW_SEMIBOLD);
    const auto previous = SelectObject(dc, heading);
    RECT title{bounds.left + Left, bounds.top + 15, bounds.right - 20, bounds.top + 40};
    DrawTextW(dc, L"CARGO CONTROL ETS2", -1, &title, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, previous); DeleteObject(heading);

    HFONT body = Font(20, FW_NORMAL);
    SelectObject(dc, body); SetTextColor(dc, Foreground);
    RECT text{bounds.left + Left, bounds.top + BodyTop,
        bounds.left + BodyRight(bounds.right - bounds.left), bounds.bottom - 20};
    DrawTextW(dc, message.c_str(), -1, &text, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
    SelectObject(dc, previous); DeleteObject(body);

    if (logo) {
        const int height = static_cast<int>(LogoWidth * logo->GetHeight() / logo->GetWidth());
        Gdiplus::Graphics graphics(dc);
        graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
        graphics.DrawImage(logo, Gdiplus::Rect(bounds.right - LogoWidth - 20,
            bounds.bottom - height - 16, LogoWidth, height));
    }
}

void Shutdown() {
    delete logo; logo = nullptr;
    if (logo_stream) logo_stream->Release();
    logo_stream = nullptr;
    if (token) Gdiplus::GdiplusShutdown(token);
    token = 0;
}
}
