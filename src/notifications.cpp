#include "notifications.h"
#include "branding.h"
#include "overlay_ui.h"
#include <atomic>
#include <Windows.h>
#include <ShlObj.h>
#include <shellapi.h>
#include <fstream>
#include <sstream>

namespace notifications {
namespace {
std::filesystem::path home;


std::atomic<bool> russian{false};
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
    branding::Initialize();

}
bool Russian() { return russian.load(); }
std::wstring Translate(const wchar_t*, const wchar_t* english, const wchar_t* russian_text) {
    DetectLanguage();
    return russian ? russian_text : english;
}
void Show(const std::wstring& message) { overlay_ui::Notify(message); }
void Tick() {}
void Shutdown() { branding::Shutdown(); }
}
