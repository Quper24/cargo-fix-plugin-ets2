#include "notifications.h"
#include <Windows.h>
#include <fstream>
#include <cstdio>
#include <cstdlib>
namespace branding {bool Initialize(){return true;}void Shutdown(){}}
namespace overlay_ui {void Notify(const std::wstring&) {}}
#define Check(value) do {if(!(value)){std::fprintf(stderr,"Language check failed: %d\n",__LINE__);std::exit(1);}}while(0)
int wmain(int argc,wchar_t** argv){
    Check(argc==3 && std::wstring(argv[1])==L"-homedir");
    const std::filesystem::path directory=argv[2];
    Check(std::filesystem::create_directory(directory));
    const auto config=directory/L"config.cfg",log=directory/L"game.log.txt",ini=directory/L"CargoControl.ini";
    {std::ofstream out(config);out<<"uset g_lang_init \"ru_ru\"\nuset g_lang \"en_gb\"\n";}
    notifications::Initialize(directory);Check(!notifications::Russian());
    {std::ofstream out(log);out<<"00:00:01 : Selected language: ru_ru\n";}
    notifications::Tick();Check(notifications::Russian());
    {std::ofstream out(log,std::ios::app);out<<"00:00:02 : Selected language: de_de\n";}
    Sleep(1050);notifications::Tick();Check(!notifications::Russian());
    Check(notifications::SetLanguage(interface_language::Choice::Russian));Check(notifications::Russian());
    notifications::Shutdown();notifications::Initialize(directory);
    Check(notifications::Language()==interface_language::Choice::Russian && notifications::Russian());
    Check(notifications::SetLanguage(interface_language::Choice::English));Check(!notifications::Russian());
    Check(notifications::SetLanguage(interface_language::Choice::Auto));notifications::Tick();Check(!notifications::Russian());
    // Truncated log and partial lines must not retain the old selection.
    {std::ofstream out(log);out<<"Selected language: RU";}
    Sleep(1050);notifications::Tick();Check(!notifications::Russian());
    {std::ofstream out(log,std::ios::app);out<<"_RU\n";}
    Sleep(1050);notifications::Tick();Check(notifications::Russian());
    Check(interface_language::IsRussian("ru-RU"));Check(!interface_language::IsRussian("russian"));
    std::istringstream fallback(" # uset g_lang \"en_gb\"\nuset g_lang_init \"ru_ru\"\n");
    Check(interface_language::FromConfig(fallback)=="ru_ru");
    notifications::Shutdown();
    Check(std::filesystem::remove(config));Check(std::filesystem::remove(log));Check(std::filesystem::remove(ini));Check(std::filesystem::remove(directory));
    std::puts("Language auto detection, live changes, log truncation, manual override and persistence passed.");
}
