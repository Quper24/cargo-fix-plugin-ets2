#include "branding.h"
#include <objidl.h>
#include <gdiplus.h>
#include <vector>
#include <iostream>

int wmain(int argc, wchar_t** argv) {
    if (!branding::Initialize()) { std::wcerr << L"Embedded logo failed to load\n"; return 1; }
    const std::wstring message = L"Рынок грузоперевозок / Заказы агентств\nОбновлено заказов: 12 480. Доступно запусков: 2 из 3.";
    HDC dc = CreateCompatibleDC(nullptr);
    const int height = branding::Measure(dc, message);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = branding::PanelWidth;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* pixels = nullptr;
    HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (!dc || !bitmap) { branding::Shutdown(); return 2; }
    const auto previous = SelectObject(dc, bitmap);
    const RECT bounds{0, 0, branding::PanelWidth, height};
    branding::Paint(dc, bounds, message);
    GdiFlush();
    SelectObject(dc, previous);
    int result = 3;
    {
        Gdiplus::Bitmap image(bitmap, nullptr);
        UINT count = 0, size = 0;
        Gdiplus::GetImageEncodersSize(&count, &size);
        std::vector<unsigned char> storage(size);
        auto* encoders = reinterpret_cast<Gdiplus::ImageCodecInfo*>(storage.data());
        Gdiplus::GetImageEncoders(count, size, encoders);
        for (UINT i = 0; i < count; ++i) if (wcscmp(encoders[i].MimeType, L"image/png") == 0) {
            result = image.Save(argc > 1 ? argv[1] : L"notification-preview.png", &encoders[i].Clsid) == Gdiplus::Ok ? 0 : 4;
            break;
        }
    }
    DeleteObject(bitmap); DeleteDC(dc);
    branding::Shutdown();
    return result;
}
