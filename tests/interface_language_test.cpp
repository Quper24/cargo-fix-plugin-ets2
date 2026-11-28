#include "notifications.h"
#include <Windows.h>
#include <fstream>
#include <cstdio>
#include <cstdlib>
#include <regex>
#include <vector>
namespace branding {bool Initialize(){return true;}void Shutdown(){}}
namespace overlay_ui {void Notify(const std::wstring&) {}}
#define Check(value) do {if(!(value)){std::fprintf(stderr,"Language check failed: %d\n",__LINE__);std::exit(1);}}while(0)
int wmain(int argc,wchar_t** argv){
    static_assert(std::size(interface_language::Codes)==static_cast<unsigned>(interface_language::Choice::Count));
    static_assert(std::size(interface_language::Names)==static_cast<unsigned>(interface_language::Choice::Count));
    const char* required[]={
#include "translation_keys.inc"
    };
    const std::regex format(R"(%(?:0?\d+)?(?:ll|z)?[udi])");
    auto formats=[&](const std::string& value){
        std::vector<std::string> result;
        for(auto it=std::sregex_iterator(value.begin(),value.end(),format);it!=std::sregex_iterator();++it)result.push_back(it->str());
        return result;
    };
    for(const auto* key:required){
        const auto found=localization::Catalog().find(key);
        if(found==localization::Catalog().end())std::fprintf(stderr,"Missing translation key: %s\n",key);
        Check(found!=localization::Catalog().end());
        for(const auto& text:found->second){
            Check(!text.empty());
            Check(MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.c_str(),-1,nullptr,0)>0);
            if(formats(key)!=formats(text))std::fprintf(stderr,"Format mismatch: %s -> %s\n",key,text.c_str());
            Check(formats(key)==formats(text));
        }
    }
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
    for(int i=3;i<static_cast<int>(interface_language::Choice::Count);++i){
        const auto language=static_cast<interface_language::Choice>(i);
        Check(notifications::SetLanguage(language));
        Check(localization::active==language);
        Check(std::string(localization::Text("Settings","Налады"))==localization::Catalog().at("Settings")[i-3]);
        notifications::Shutdown();notifications::Initialize(directory);
        Check(notifications::Language()==language && localization::active==language);
    }
    Check(!notifications::SetLanguage(interface_language::Choice::Count));
    Check(interface_language::FromCode("kk_KZ")==interface_language::Choice::Kazakh);
    Check(interface_language::FromCode("pt-BR")==interface_language::Choice::Portuguese);
    Check(interface_language::FromCode("cs_cz")==interface_language::Choice::Czech);
    Check(interface_language::FromCode("uk_UA")==interface_language::Choice::Ukrainian);
    Check(interface_language::FromCode("ua")==interface_language::Choice::Ukrainian);
    Check(static_cast<int>(interface_language::Choice::Ukrainian)==static_cast<int>(interface_language::Choice::Belarusian)+1);
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
