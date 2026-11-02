#include "control_menu.h"
#include <Windows.h>
#include <commctrl.h>
#include <filesystem>
#include <cstdlib>
#include <cstdio>
#include <vector>
#include <string>
// Locale provider only; the real native menu and controls are exercised.
namespace notifications { bool Russian() { return false; } }
#define Check(ok) do { if (!(ok)) { std::fprintf(stderr,"Menu check failed at line %d, error=%lu\n",__LINE__,GetLastError()); std::exit(1); } } while (0)
BOOL CALLBACK Collect(HWND h, LPARAM data) {
    wchar_t cls[100]{}; GetClassNameW(h, cls, 100);
    if (std::wstring(cls) == HOTKEY_CLASSW) reinterpret_cast<std::vector<HWND>*>(data)->push_back(h);
    return TRUE;
}

HWND OwnMenu() {
    HWND found = nullptr;
    EnumThreadWindows(GetCurrentThreadId(), [](HWND h, LPARAM data)->BOOL {
        wchar_t name[100]{}; GetClassNameW(h,name,100);
        if (std::wstring(name) == L"CargoControlETS2Menu") { *reinterpret_cast<HWND*>(data)=h; return FALSE; }
        return TRUE;
    },reinterpret_cast<LPARAM>(&found));
    return found;
}
int main() {
    const auto dir = std::filesystem::temp_directory_path() / (L"CargoControlMenuCheck-" + std::to_wstring(GetCurrentProcessId()));
    std::filesystem::create_directories(dir);
    const auto ini = dir / L"CargoControlETS2.ini";
    Check(WritePrivateProfileStringW(L"General", L"HotkeyVirtualKey", L"119", ini.c_str()));
    control_menu::Initialize(dir);
    HWND menu = OwnMenu();
    Check(menu && !IsWindowVisible(menu));
    std::vector<HWND> fields;
    EnumChildWindows(menu, Collect, reinterpret_cast<LPARAM>(&fields));
    Check(fields.size() == 2);
    Check(!SendMessageW(fields[0], HKM_GETHOTKEY, 0, 0));
    Check(!SendMessageW(fields[1], HKM_GETHOTKEY, 0, 0));
    SendMessageW(fields[0], HKM_SETHOTKEY, 0x377, 0);
    SendMessageW(fields[1], HKM_SETHOTKEY, 0x378, 0);
    SendMessageW(menu, WM_COMMAND, 105, 0); // Save actual native controls to INI.
    Check(GetPrivateProfileIntW(L"Bindings", L"Cargo", 0, ini.c_str()) == 0x377);
    Check(GetPrivateProfileIntW(L"Bindings", L"Freight", 0, ini.c_str()) == 0x378);
    control_menu::Shutdown();
    control_menu::Initialize(dir);
    menu = OwnMenu();
    fields.clear(); EnumChildWindows(menu, Collect, reinterpret_cast<LPARAM>(&fields));
    Check(fields.size() == 2);
    Check(SendMessageW(fields[0], HKM_GETHOTKEY, 0, 0) == 0x377);
    Check(SendMessageW(fields[1], HKM_GETHOTKEY, 0, 0) == 0x378);
    SendMessageW(menu, WM_COMMAND, 103, 0);
    SendMessageW(menu, WM_COMMAND, 104, 0);
    SendMessageW(menu, WM_COMMAND, 105, 0);
    Check(!GetPrivateProfileIntW(L"Bindings", L"Cargo", 1, ini.c_str()));
    Check(!GetPrivateProfileIntW(L"Bindings", L"Freight", 1, ini.c_str()));
    control_menu::Shutdown();
    Check(!IsWindow(menu));
    std::filesystem::remove(ini);
    std::filesystem::remove(dir);
}
