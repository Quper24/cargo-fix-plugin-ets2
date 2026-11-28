#pragma once
#include <algorithm>
#include <cctype>
#include <sstream>
#include <string>

namespace interface_language {
enum class Choice { Auto, Russian, English, Polish, Czech, German, Spanish, Italian, Lithuanian, Estonian,
                    Belarusian, Ukrainian, Latvian, Kazakh, Kyrgyz, Uzbek, Azerbaijani, Romanian, Portuguese, Count };
inline constexpr unsigned short GlyphRanges[]={0x20,0x2af,0x400,0x52f,0x1e00,0x1eff,0x2000,0x206f,0x20ac,0x20ac,0x2116,0x2116,0};
inline constexpr const char* Codes[]={"auto","ru","en","pl","cs","de","es","it","lt","et","be","uk","lv","kk","ky","uz","az","ro","pt"};
inline constexpr const char* Names[]={"Auto","Русский","English","Polski","Čeština","Deutsch","Español","Italiano",
    "Lietuvių","Eesti","Беларуская","Українська","Latviešu","Қазақша","Кыргызча","O‘zbekcha","Azərbaycanca","Română","Português"};
inline Choice FromCode(std::string value){
    std::transform(value.begin(),value.end(),value.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});
    const auto separator=value.find_first_of("_-");if(separator!=std::string::npos)value.resize(separator);
    if(value=="cz")value="cs";
    if(value=="by")value="be";
    if(value=="ua")value="uk";
    for(int i=0;i<static_cast<int>(Choice::Count);++i)if(value==Codes[i])return static_cast<Choice>(i);
    return Choice::Auto;
}
inline bool IsRussian(std::string value){
    std::transform(value.begin(),value.end(),value.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});
    return value=="ru" || value.rfind("ru_",0)==0 || value.rfind("ru-",0)==0;
}
inline std::string FromConfig(std::istream& input){
    std::string line,initial,current;
    while(std::getline(input,line)){
        std::istringstream fields(line);std::string command,key,value;
        if(!(fields>>command>>key) || command!="uset")continue;
        fields>>std::ws;if(fields.peek()!='"')continue;
        fields.get();std::getline(fields,value,'"');
        if(key=="g_lang")current=value;
        else if(key=="g_lang_init")initial=value;
    }
    return current.empty()?initial:current;
}
inline std::string FromLogLine(const std::string& line){
    constexpr char marker[]="Selected language: ";
    const auto at=line.find(marker);if(at==std::string::npos)return {};
    std::istringstream fields(line.substr(at+sizeof(marker)-1));std::string value;fields>>value;return value;
}
inline bool Russian(Choice choice,bool detected){return choice==Choice::Russian || (choice==Choice::Auto && detected);}
}
