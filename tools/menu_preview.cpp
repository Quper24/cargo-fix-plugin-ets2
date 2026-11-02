#include "branding.h"
#include "control_menu.h"
#include "menu_style.h"
namespace notifications { bool Russian() { return true; } }
#include <objidl.h>
#include <gdiplus.h>
#include <vector>
#include <iostream>


HWND OwnMenu() {
    HWND found = nullptr;
    EnumThreadWindows(GetCurrentThreadId(), [](HWND h, LPARAM data)->BOOL {
        wchar_t name[100]{}; GetClassNameW(h,name,100);
        if (std::wstring(name) == L"CargoControlETS2Menu") { *reinterpret_cast<HWND*>(data)=h; return FALSE; }
        return TRUE;
    },reinterpret_cast<LPARAM>(&found));
    return found;
}
int wmain(int argc, wchar_t** argv) {
    if (!branding::Initialize()) { std::wcerr << L"Embedded logo failed to load\n"; return 1; }
    const std::wstring message = L"Рынок грузоперевозок / Заказы агентств\nОбновлено заказов: 12 480. Доступно запусков: 2 из 3.";
    HDC dc = CreateCompatibleDC(nullptr);
    const int height = menu_style::Height;
    control_menu::Initialize(std::filesystem::temp_directory_path());
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = menu_style::Width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* pixels = nullptr;
    HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (!dc || !bitmap) { branding::Shutdown(); return 2; }
    const auto previous = SelectObject(dc, bitmap);
    const RECT bounds{0, 0, menu_style::Width, height};
    HWND menu = OwnMenu();
    SendMessageW(menu,WM_PRINT,reinterpret_cast<WPARAM>(dc),PRF_CLIENT|PRF_CHILDREN|PRF_ERASEBKGND);
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
    control_menu::Shutdown();
    branding::Shutdown();
    return result;
}
