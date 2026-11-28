#pragma once
#include "interface_language.h"
#include <array>
#include <atomic>
#include <map>
#include <string_view>

namespace localization {
inline std::atomic<interface_language::Choice> active{interface_language::Choice::English};
using Translations=std::array<std::string,16>;
inline const std::map<std::string,Translations>& Catalog(){
    static const std::map<std::string,Translations> catalog=[] {
        std::map<std::string,Translations> result{
#include "translations.inc"
        };
        const std::pair<const char*,const char*> aliases[]={
#include "translation_aliases.inc"
        };
        for(const auto& alias:aliases)result.emplace(alias.first,result.at(alias.second));
        auto compose=[&](const char* key,const char* first,const char* separator,const char* second,const char* ending){
            Translations values;
            for(unsigned i=0;i<values.size();++i)values[i]=result.at(first)[i]+separator+(second?result.at(second)[i]:std::string{})+ending;
            result.emplace(key,std::move(values));
        };
        compose("Data ready. Companies: %llu   Cargos: %llu","Companies",": %llu   ","Cargos",": %llu");
        compose("Companies processed: %llu / %llu","Companies",": %llu / %llu",nullptr,"");
        compose("Offers processed: %llu / %llu","Offers",": %llu / %llu",nullptr,"");
        compose("Stations processed: %llu / %llu","Stations",": %llu / %llu",nullptr,"");
        compose("Stations: %zu  |  Route: %zu / 24","Stations",": %zu  |  ","Route",": %zu / 24");
        compose("Unreadable entries skipped: %llu","Skipped",": %llu",nullptr,"");
        compose("Cargo market refreshed\nNew offers: %llu. Available runs: %u of 3.","New offers",": %llu. ","Available runs",": %u / 3.");
        compose("Freight market / Quick jobs\nOrders refreshed: %llu. You can refresh again in 5 s.","Offers",": %llu. ","Refresh orders"," — 5 s.");
        compose("\nCompanies with five new offers: %llu of %llu.","Companies"," (5): %llu / %llu.",nullptr,"");
        compose(" - departure","Departure city","",nullptr,"");
        compose(" - final stop","Destination city","",nullptr,"");
        compose("Road Trip: refreshed %llu, preserved %llu. Reopen the Road Trip market.","New offers",": %llu. ","Preserved",": %llu. Road Trip.");
        for(const auto& pair:std::array<std::pair<const char*,const char*>,4>{{
            {"Clear##cargo","Clear"},{"Clear##freight","Clear"},{"Default##menu","Default"},{"Reset##scale","Reset"}}}){
            auto values=result.at(pair.second);
            const std::string key=pair.first,suffix=key.substr(key.find("##"));
            for(auto& value:values)value+=suffix;
            result.emplace(key,std::move(values));
        }
        return result;
    }();
    return catalog;
}
inline const char* Text(const char* english,const char* russian,bool prefer_russian=false){
    const auto language=active.load();
    if(language==interface_language::Choice::Russian)return russian;
    if(language<=interface_language::Choice::English)return prefer_russian?russian:english;
    const auto found=Catalog().find(english);
    if(found==Catalog().end())return english;
    return found->second[static_cast<unsigned>(language)-3].c_str();
}
}
