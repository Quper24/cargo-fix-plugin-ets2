#include "notifications.h"
#include <Windows.h>
#include <ShlObj.h>
#include <shellapi.h>
#include <fstream>
#include <sstream>

namespace notifications {
namespace {
HWND window = nullptr;
std::wstring text;
ULONGLONG until = 0;
std::filesystem::path home;


bool russian = false;
int panel_height = 150;
HFONT MessageFont() {
    return CreateFontW(-22, 0, 0, 0, FW_MEDIUM, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH, L"Segoe UI");
}
LRESULT CALLBACK Procedure(HWND hwnd, UINT message, WPARAM w, LPARAM l) {
    if (message == WM_NCHITTEST) return HTTRANSPARENT;
    if (message == WM_PAINT) {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(hwnd, &paint);
        RECT rect{}; GetClientRect(hwnd, &rect);
        HBRUSH brush = CreateSolidBrush(RGB(25, 30, 40));
        FillRect(dc, &rect, brush); DeleteObject(brush);
        SetBkMode(dc, TRANSPARENT); SetTextColor(dc, RGB(255, 235, 180));
        HFONT font = MessageFont();
        auto previous = SelectObject(dc, font);
        InflateRect(&rect, -18, -12);
        DrawTextW(dc, text.c_str(), -1, &rect, DT_CENTER | DT_WORDBREAK | DT_NOPREFIX);
        SelectObject(dc, previous); DeleteObject(font);
        EndPaint(hwnd, &paint); return 0;
    }
    return DefWindowProcW(hwnd, message, w, l);
}
void DetectLanguage() {
    std::ifstream config(home / L"config.cfg");
    std::string line, language;
    while (std::getline(config, line)) {
        if (line.find("uset g_lang ") != std::string::npos ||
            (language.empty() && line.find("uset g_lang_init ") != std::string::npos)) {
            const auto first = line.find('"'), last = line.rfind('"');
            if (first != std::string::npos && last > first) language = line.substr(first + 1, last - first - 1);
        }
    }
    russian = language.rfind("ru", 0) == 0;

}
}
void Initialize(const std::filesystem::path&) {

    PWSTR documents = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &documents))) {
        home = std::filesystem::path(documents) / L"Euro Truck Simulator 2";
        CoTaskMemFree(documents);
    }
    int count = 0;
    auto args = CommandLineToArgvW(GetCommandLineW(), &count);
    if (args) {
        for (int i = 1; i + 1 < count; ++i)
            if (_wcsicmp(args[i], L"-homedir") == 0) home = args[i + 1];
        LocalFree(args);
    }
    DetectLanguage();
    WNDCLASSW cls{};
    cls.lpfnWndProc = Procedure; cls.hInstance = GetModuleHandleW(nullptr);
    cls.lpszClassName = L"CargoControlETS2Notification";
    RegisterClassW(&cls);
    window = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
        cls.lpszClassName, L"Cargo Control ETS2", WS_POPUP, 0, 0, 800, 150, nullptr, nullptr, cls.hInstance, nullptr);
    if (window) SetLayeredWindowAttributes(window, 0, 240, LWA_ALPHA);
}
bool Russian() { DetectLanguage(); return russian; }
std::wstring Translate(const wchar_t*, const wchar_t* english, const wchar_t* russian_text) {
    DetectLanguage();
    return russian ? russian_text : english;
}
void Show(const std::wstring& message) {
    text = message; until = GetTickCount64() + 7000;
    if (window) {
        HDC dc = GetDC(window);
        if (dc) {
            HFONT font = MessageFont();
            const auto previous = SelectObject(dc, font);
            RECT bounds{0, 0, 764, 0};
            DrawTextW(dc, text.c_str(), -1, &bounds, DT_CALCRECT | DT_CENTER | DT_WORDBREAK | DT_NOPREFIX);
            panel_height = bounds.bottom + 24;
            if (panel_height < 110) panel_height = 110;
            if (panel_height > 260) panel_height = 260;
            SelectObject(dc, previous); DeleteObject(font); ReleaseDC(window, dc);
        }
        InvalidateRect(window, nullptr, TRUE);
    }
    Tick();
}
void Tick() {
    if (!window) return;
    HWND foreground = GetForegroundWindow();
    DWORD process = 0; GetWindowThreadProcessId(foreground, &process);
    if (GetTickCount64() >= until || process != GetCurrentProcessId()) {
        ShowWindow(window, SW_HIDE); return;
    }
    RECT rect{}; GetWindowRect(foreground, &rect);
    SetWindowPos(window, HWND_TOPMOST, rect.left + (rect.right - rect.left - 800) / 2,
        rect.top + 55, 800, panel_height, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    UpdateWindow(window);
}
void Shutdown() {
    if (window) DestroyWindow(window);
    window = nullptr;
    UnregisterClassW(L"CargoControlETS2Notification", GetModuleHandleW(nullptr));
}
}

