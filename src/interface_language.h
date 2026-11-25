#pragma once
#include <algorithm>
#include <cctype>
#include <sstream>
#include <string>

namespace interface_language {
enum class Choice { Auto, Russian, English };
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
