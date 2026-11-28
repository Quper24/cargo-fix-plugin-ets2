#include "branding.h"
#include "brand_palette.h"
#include <objidl.h>
#include <gdiplus.h>
#include <algorithm>
#include <cstring>
#include <cstddef>

namespace branding {
namespace {
ULONG_PTR token = 0;
Gdiplus::Image* logo = nullptr;
Gdiplus::Rect logo_bounds;
IStream* logo_stream = nullptr;
constexpr COLORREF Background = RGB(0, 0, 0);
constexpr COLORREF Accent = RGB(brand_palette::Red, brand_palette::Green, brand_palette::Blue);
constexpr COLORREF Border = RGB(65, 65, 65);
constexpr COLORREF Foreground = RGB(255, 255, 255);
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
    // Keep the supplied logo's colours. Measure the artwork so its empty canvas
    // does not shrink the mark inside the menu's logo panel.
    auto* tinted=new Gdiplus::Bitmap(logo->GetWidth(),logo->GetHeight(),PixelFormat32bppARGB);
    if(tinted->GetLastStatus()!=Gdiplus::Ok){delete tinted;Shutdown();return false;}
    {
        Gdiplus::Graphics graphics(tinted);
        graphics.SetCompositingMode(Gdiplus::CompositingModeSourceCopy);
        graphics.DrawImage(logo,0,0,static_cast<INT>(logo->GetWidth()),static_cast<INT>(logo->GetHeight()));
    }
    Gdiplus::Rect area(0,0,tinted->GetWidth(),tinted->GetHeight());
    Gdiplus::BitmapData pixels{};
    if(tinted->LockBits(&area,Gdiplus::ImageLockModeRead|Gdiplus::ImageLockModeWrite,PixelFormat32bppARGB,&pixels)!=Gdiplus::Ok){delete tinted;Shutdown();return false;}
    UINT left=pixels.Width,top=pixels.Height,right=0,bottom=0;
    for(UINT y=0;y<pixels.Height;++y){
        auto* row=static_cast<unsigned char*>(pixels.Scan0)+std::ptrdiff_t(y)*pixels.Stride;
        for(UINT x=0;x<pixels.Width;++x){
            auto* p=row+x*4;
            if(p[3] && std::max({p[0],p[1],p[2]})>24){
                left=std::min(left,x);top=std::min(top,y);
                right=std::max(right,x);bottom=std::max(bottom,y);
            }
        }
    }
    tinted->UnlockBits(&pixels);delete logo;logo=tinted;
    logo_bounds=right>=left&&bottom>=top?Gdiplus::Rect(left,top,right-left+1,bottom-top+1):area;
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
    DrawTextW(dc, L"CARGO CONTROL", -1, &title, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);
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

void PaintLogo(HDC dc, const RECT& bounds) {
    if (!logo) return;
    const float scale=std::min(float(bounds.right-bounds.left)/logo_bounds.Width,
                               float(bounds.bottom-bounds.top)/logo_bounds.Height);
    const int width=static_cast<int>(logo_bounds.Width*scale);
    const int height=static_cast<int>(logo_bounds.Height*scale);
    const int x=bounds.left+(bounds.right-bounds.left-width)/2;
    const int y=bounds.top+(bounds.bottom-bounds.top-height)/2;
    Gdiplus::Graphics graphics(dc);
    graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
    graphics.DrawImage(logo,Gdiplus::Rect(x,y,width,height),logo_bounds.X,logo_bounds.Y,
                       logo_bounds.Width,logo_bounds.Height,Gdiplus::UnitPixel);
}

bool LogoPixels(unsigned width,unsigned height,std::vector<unsigned char>& rgba){
    if(!logo || !width || !height)return false;
    Gdiplus::Bitmap bitmap(width,height,PixelFormat32bppARGB);
    {
        Gdiplus::Graphics graphics(&bitmap);graphics.Clear(Gdiplus::Color(0,0,0,0));
        graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        const float scale=std::min(float(width)/logo_bounds.Width,float(height)/logo_bounds.Height);
        const int w=static_cast<int>(logo_bounds.Width*scale),h=static_cast<int>(logo_bounds.Height*scale);
        if(graphics.DrawImage(logo,Gdiplus::Rect((width-w)/2,(height-h)/2,w,h),logo_bounds.X,logo_bounds.Y,
            logo_bounds.Width,logo_bounds.Height,Gdiplus::UnitPixel)!=Gdiplus::Ok)return false;
    }
    Gdiplus::Rect area(0,0,width,height);Gdiplus::BitmapData pixels{};
    if(bitmap.LockBits(&area,Gdiplus::ImageLockModeRead,PixelFormat32bppARGB,&pixels)!=Gdiplus::Ok)return false;
    rgba.resize(std::size_t(width)*height*4);
    for(unsigned y=0;y<height;++y){
        const auto* row=static_cast<const unsigned char*>(pixels.Scan0)+std::ptrdiff_t(y)*pixels.Stride;
        for(unsigned x=0;x<width;++x){
            const auto* p=row+x*4;auto* out=rgba.data()+(std::size_t(y)*width+x)*4;
            out[0]=p[2];out[1]=p[1];out[2]=p[0];out[3]=p[3];
        }
    }
    bitmap.UnlockBits(&pixels);return true;
}
void Shutdown() {
    delete logo; logo = nullptr;
    if (logo_stream) logo_stream->Release();
    logo_stream = nullptr;
    if (token) Gdiplus::GdiplusShutdown(token);
    token = 0;
}
}
