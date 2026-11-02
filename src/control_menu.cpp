#include "control_menu.h"
#include "notifications.h"
#include "hotkey_binding.h"
#include "menu_style.h"
#include <Windows.h>
#include <commctrl.h>
#include <string>

namespace control_menu {
namespace {
HWND window = nullptr, cargo_edit = nullptr, freight_edit = nullptr, game_window = nullptr;
std::filesystem::path ini;
WORD cargo = 0, freight = 0;
bool menu_down = false, cargo_down = false, freight_down = false;
int pending = 0;
HFONT ui_font = nullptr;
HBRUSH field_brush = nullptr;
const wchar_t* Tr(const wchar_t* en, const wchar_t* ru) { return notifications::Russian() ? ru : en; }
bool Down(int key) { return (GetAsyncKeyState(key) & 0x8000) != 0; }
bool Held(WORD binding) {
    return !Down(VK_LWIN) && !Down(VK_RWIN) && hotkeys::Matches(binding, (binding & 255) && Down(LOBYTE(binding)),
        Down(VK_CONTROL), Down(VK_SHIFT), Down(VK_MENU));
}
WORD Valid(unsigned value) { return hotkeys::Valid(value); }
void Close() {
    ShowWindow(window, SW_HIDE);
    if (game_window && IsWindow(game_window)) SetForegroundWindow(game_window);
}
void Request(int action) { pending = action; Close(); }
LRESULT CALLBACK Procedure(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
    if (msg == WM_ERASEBKGND) return 1;
    if (msg == WM_PRINTCLIENT) { menu_style::Paint(reinterpret_cast<HDC>(w), notifications::Russian()); return 0; }
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps{}; HDC dc = BeginPaint(hwnd, &ps);
        HDC buffer = CreateCompatibleDC(dc); HBITMAP bmp = CreateCompatibleBitmap(dc, menu_style::Width, menu_style::Height);
        auto old = SelectObject(buffer, bmp); menu_style::Paint(buffer, notifications::Russian());
        BitBlt(dc,0,0,menu_style::Width,menu_style::Height,buffer,0,0,SRCCOPY);
        SelectObject(buffer,old); DeleteObject(bmp); DeleteDC(buffer); EndPaint(hwnd,&ps); return 0;
    }
    if (msg == WM_DRAWITEM) { menu_style::Button(*reinterpret_cast<DRAWITEMSTRUCT*>(l)); return TRUE; }
    if (msg == WM_CTLCOLOREDIT || msg == WM_CTLCOLORSTATIC) {
        HDC dc = reinterpret_cast<HDC>(w); SetTextColor(dc,RGB(235,244,237));
        SetBkColor(dc,menu_style::Surface); return reinterpret_cast<LRESULT>(field_brush);
    }
    if (msg == WM_NCHITTEST) {
        POINT pt{static_cast<short>(LOWORD(l)),static_cast<short>(HIWORD(l))}; ScreenToClient(hwnd,&pt);
        if (pt.y < 140) return HTCAPTION;
    }
    if (msg == WM_CLOSE) { Close(); return 0; }
    if (msg == WM_COMMAND) {
        switch (LOWORD(w)) {
        case 101: Request(1); return 0;
        case 102: Request(2); return 0;
        case 103: SendMessageW(cargo_edit, HKM_SETHOTKEY, 0, 0); return 0;
        case 104: SendMessageW(freight_edit, HKM_SETHOTKEY, 0, 0); return 0;
        case 105: {
            const auto a = Valid(static_cast<unsigned>(SendMessageW(cargo_edit, HKM_GETHOTKEY, 0, 0)));
            const auto b = Valid(static_cast<unsigned>(SendMessageW(freight_edit, HKM_GETHOTKEY, 0, 0)));
            if (!hotkeys::PairAllowed(a, b)) {
                MessageBoxW(hwnd, Tr(L"Choose different shortcuts. Ctrl+Alt+F9 opens this menu.", L"Назначьте разные сочетания. Ctrl+Alt+F9 открывает это меню."), L"Cargo Control ETS2", MB_OK); return 0;
            }
            // Single write persists both bindings together; no partial saved pair.
            wchar_t values[160];
            const int n = swprintf_s(values, L"Cargo=%u%cFreight=%u%c", unsigned(a), 0, unsigned(b), 0);
            if (n < 0 || !WritePrivateProfileSectionW(L"Bindings", values, ini.c_str())) {
                MessageBoxW(hwnd, Tr(L"Could not save settings. Check write access to the plugin folder.", L"Не удалось сохранить настройки. Проверьте доступ на запись в папку плагина."), L"Cargo Control ETS2", MB_OK | MB_ICONERROR); return 0;
            }
            cargo = a; freight = b; Close(); return 0;
        }
        case 106: Close(); return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, w, l);
}
LRESULT CALLBACK ButtonHover(HWND hwnd, UINT msg, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR) {
    if (msg == WM_MOUSEMOVE) { TRACKMOUSEEVENT track{sizeof(track),TME_LEAVE,hwnd,0}; TrackMouseEvent(&track); InvalidateRect(hwnd,nullptr,FALSE); }
    if (msg == WM_MOUSELEAVE || msg == WM_SETFOCUS || msg == WM_KILLFOCUS) InvalidateRect(hwnd,nullptr,FALSE);
    return DefSubclassProc(hwnd,msg,w,l);
}
LRESULT CALLBACK HotkeyPaint(HWND hwnd, UINT msg, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR) {
    if (msg == WM_ERASEBKGND) return 1;
    if (msg == WM_PAINT || msg == WM_PRINT || msg == WM_PRINTCLIENT) {
        PAINTSTRUCT ps{}; HDC dc = msg == WM_PAINT ? BeginPaint(hwnd,&ps) : reinterpret_cast<HDC>(w);
        RECT rect{}; GetClientRect(hwnd,&rect);
        const auto binding=static_cast<WORD>(SendMessageW(hwnd,HKM_GETHOTKEY,0,0));
        menu_style::Hotkey(dc,rect,binding,GetFocus()==hwnd,notifications::Russian());
        if (msg == WM_PAINT) EndPaint(hwnd,&ps); return 0;
    }
    const auto result=DefSubclassProc(hwnd,msg,w,l);
    if (msg==WM_KEYDOWN || msg==WM_SYSKEYDOWN || msg==WM_KEYUP || msg==WM_SYSKEYUP ||
        msg==HKM_SETHOTKEY || msg==WM_SETFOCUS || msg==WM_KILLFOCUS) InvalidateRect(hwnd,nullptr,FALSE);
    return result;
}
HWND Control(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int width, int height, int id) {
    HWND h = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, width, height, window,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(ui_font), TRUE);
    if (wcscmp(cls,L"BUTTON") == 0) SetWindowSubclass(h,ButtonHover,1,0);
    if (wcscmp(cls,HOTKEY_CLASSW)==0) SetWindowSubclass(h,HotkeyPaint,2,0);
    return h;
}
void Open() {
    game_window = GetForegroundWindow();
    SendMessageW(cargo_edit, HKM_SETHOTKEY, cargo, 0);
    SendMessageW(freight_edit, HKM_SETHOTKEY, freight, 0);
    RECT rect{}; GetWindowRect(game_window, &rect);
    SetWindowPos(window, HWND_TOPMOST, rect.left + (rect.right-rect.left-menu_style::Width)/2,
        rect.top + (rect.bottom-rect.top-menu_style::Height)/2, menu_style::Width, menu_style::Height, SWP_SHOWWINDOW);
    SetForegroundWindow(window); SetFocus(window);
}
}
void Initialize(const std::filesystem::path& directory) {
    ini = directory / L"CargoControlETS2.ini";
    cargo = Valid(GetPrivateProfileIntW(L"Bindings", L"Cargo", 0, ini.c_str()));
    freight = Valid(GetPrivateProfileIntW(L"Bindings", L"Freight", 0, ini.c_str()));
    const WORD reserved = MAKEWORD(VK_F9, HOTKEYF_CONTROL | HOTKEYF_ALT);
    if (cargo == reserved) cargo = 0;
    if (freight == reserved || (cargo && freight == cargo)) freight = 0;
    INITCOMMONCONTROLSEX cc{sizeof(cc), ICC_HOTKEY_CLASS}; InitCommonControlsEx(&cc);
    WNDCLASSW cls{}; cls.lpfnWndProc = Procedure; cls.hInstance = GetModuleHandleW(nullptr);
    cls.lpszClassName = L"CargoControlETS2Menu"; cls.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    RegisterClassW(&cls);
    field_brush = CreateSolidBrush(menu_style::Surface);
    ui_font = CreateFontW(-16,0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");
    window = CreateWindowExW(WS_EX_TOOLWINDOW, cls.lpszClassName, L"Cargo Control ETS2", WS_POPUP | WS_CLIPCHILDREN,
        0, 0, menu_style::Width, menu_style::Height, nullptr, nullptr, cls.hInstance, nullptr);
    if (!window) return;
    SetWindowRgn(window,CreateRoundRectRgn(0,0,menu_style::Width+1,menu_style::Height+1,24,24),FALSE);
    Control(L"BUTTON", L"×", BS_OWNERDRAW | WS_TABSTOP, 850, 18, 30, 30, 106);
    Control(L"BUTTON", Tr(L"Add cargo", L"Добавить грузы"), BS_OWNERDRAW | WS_TABSTOP, 48, 298, 370, 42, 101);
    Control(L"BUTTON", Tr(L"Refresh orders", L"Обновить заказы"), BS_OWNERDRAW | WS_TABSTOP, 482, 298, 370, 42, 102);
    cargo_edit = Control(HOTKEY_CLASSW, L"", WS_TABSTOP, 56, 464, 274, 26, 0);
    freight_edit = Control(HOTKEY_CLASSW, L"", WS_TABSTOP, 490, 464, 274, 26, 0);
    SendMessageW(cargo_edit, HKM_SETHOTKEY, cargo, 0);
    SendMessageW(freight_edit, HKM_SETHOTKEY, freight, 0);
    Control(L"BUTTON", Tr(L"Clear",L"Сброс"), BS_OWNERDRAW | WS_TABSTOP, 350, 458, 68, 38, 103);
    Control(L"BUTTON", Tr(L"Clear",L"Сброс"), BS_OWNERDRAW | WS_TABSTOP, 784, 458, 68, 38, 104);
    Control(L"BUTTON", Tr(L"Save", L"Сохранить"), BS_OWNERDRAW | WS_TABSTOP, 576, 588, 142, 44, 105);
    Control(L"BUTTON", Tr(L"Close", L"Закрыть"), BS_OWNERDRAW | WS_TABSTOP, 734, 588, 138, 44, 106);

}
int Tick(bool driving) {
    if (!window) return 0;
    MSG msg{};
    while (PeekMessageW(&msg, window, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE) { Close(); continue; }
        TranslateMessage(&msg); DispatchMessageW(&msg);
    }
    DWORD process = 0; GetWindowThreadProcessId(GetForegroundWindow(), &process);
    const bool focused = process == GetCurrentProcessId();
    const bool m = Held(MAKEWORD(VK_F9, HOTKEYF_CONTROL | HOTKEYF_ALT));
    const bool a = Held(cargo), b = Held(freight);
    if (focused && m && !menu_down) {
        if (IsWindowVisible(window)) Close(); else Open();
    } else if (focused && !IsWindowVisible(window) && driving) {
        if (a && !cargo_down) pending = 1;
        else if (b && !freight_down) pending = 2;
    }
    menu_down = m; cargo_down = a; freight_down = b;
    if (pending && focused && driving && !IsWindowVisible(window)) {
        const int action = pending; pending = 0; return action;
    }
    return 0;
}
void CancelPending() { pending = 0; }
void Shutdown() {
    pending = 0; cargo = freight = 0; menu_down = cargo_down = freight_down = false;
    if (window) DestroyWindow(window); window = nullptr;
    if (ui_font) DeleteObject(ui_font); ui_font = nullptr;
    if (field_brush) DeleteObject(field_brush); field_brush = nullptr;
    UnregisterClassW(L"CargoControlETS2Menu", GetModuleHandleW(nullptr));
}
}
