#include "notifications.h"
#include "branding.h"
#include "overlay_ui.h"
#include <atomic>
#include <Windows.h>
#include <ShlObj.h>
#include <shellapi.h>
#include <fstream>
#include <sstream>
#include <mutex>

namespace notifications {
namespace {
std::filesystem::path home;
std::filesystem::path settings;
std::mutex language_mutex;
std::atomic<interface_language::Choice> choice{interface_language::Choice::Auto};
ULONGLONG next_language_check=0;
std::streamoff log_offset=0;
std::string log_language,partial_log_line;
FILETIME process_started{};
std::atomic<bool> russian{false};
void DetectLanguage() {
    std::ifstream config(home / L"config.cfg");
    auto language=interface_language::FromConfig(config);
    // Follow the language actually selected in this session, including changes.
    const auto path=home/L"game.log.txt";
    WIN32_FILE_ATTRIBUTE_DATA attrs{};
    if(GetFileAttributesExW(path.c_str(),GetFileExInfoStandard,&attrs) && CompareFileTime(&attrs.ftLastWriteTime,&process_started)>=0){
        std::ifstream log(path,std::ios::binary|std::ios::ate);
        const auto size=log.tellg();
        if(size>=0){
            if(size<log_offset){log_offset=0;log_language.clear();partial_log_line.clear();}
            log.seekg(log_offset);std::string line;
            while(std::getline(log,line)){
                partial_log_line+=line;
                if(log.eof())break; // Wait for the rest of a partially written line.
                const auto selected=interface_language::FromLogLine(partial_log_line);
                if(!selected.empty())log_language=selected;
                partial_log_line.clear();
            }
            log_offset=static_cast<std::streamoff>(size);
        }
    }
    if(!log_language.empty())language=log_language;
    russian=interface_language::IsRussian(language);
}
}
void Initialize(const std::filesystem::path& plugin_dir, bool american_truck) {
    std::lock_guard<std::mutex> guard(language_mutex);
    settings=plugin_dir/L"CargoControl.ini";home.clear();
    log_offset=0;log_language.clear();partial_log_line.clear();next_language_check=0;
    FILETIME exited{},kernel{},user{};GetProcessTimes(GetCurrentProcess(),&process_started,&exited,&kernel,&user);
    wchar_t saved[16]{};GetPrivateProfileStringW(L"Interface",L"Language",L"auto",saved,16,settings.c_str());
    choice=_wcsicmp(saved,L"ru")==0?interface_language::Choice::Russian:_wcsicmp(saved,L"en")==0?interface_language::Choice::English:interface_language::Choice::Auto;
    PWSTR documents = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &documents))) {
        home = std::filesystem::path(documents) / (american_truck ? L"American Truck Simulator" : L"Euro Truck Simulator 2");
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
bool Russian() { return interface_language::Russian(choice.load(),russian.load()); }
interface_language::Choice Language(){return choice.load();}
bool SetLanguage(interface_language::Choice value){
    std::lock_guard<std::mutex> guard(language_mutex);
    const auto name=value==interface_language::Choice::Russian?L"ru":value==interface_language::Choice::English?L"en":L"auto";
    if(!WritePrivateProfileStringW(L"Interface",L"Language",name,settings.c_str()))return false;
    choice=value;next_language_check=0;return true;
}
std::wstring Translate(const wchar_t*, const wchar_t* english, const wchar_t* russian_text) {
    Tick();
    return Russian() ? russian_text : english;
}
void Show(const std::wstring& message) { overlay_ui::Notify(message); }
void Tick() {
    std::lock_guard<std::mutex> guard(language_mutex);
    const auto now=GetTickCount64();if(now<next_language_check)return;
    next_language_check=now+1000;DetectLanguage();
}
void Shutdown() { branding::Shutdown(); }
}
