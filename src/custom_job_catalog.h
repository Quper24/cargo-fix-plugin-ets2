#pragma once
#include <algorithm>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace custom_jobs {
using Reader = std::function<bool(std::uintptr_t, void*, std::size_t)>;
struct Cargo { std::string id, name; };
struct Company { std::string id, city, name; std::vector<std::string> outgoing, incoming; std::string city_name, region, region_name; };
inline bool InRegion(const Company& company,const std::string& region){
    return region.empty() || region==(company.region.empty()?"#unknown":company.region);
}
enum class LabelKind { Cargo, Company, City, RegionId, Region };
using LabelReader = std::function<std::string(LabelKind, std::uintptr_t)>;
struct Catalog {
    std::vector<Company> companies;
    std::vector<Cargo> cargos;
    std::uint64_t skipped = 0;
    std::uint64_t label_fallbacks = 0;
    std::uint64_t excluded_road_trip = 0;
};
template<class T> bool Read(const Reader& reader, std::uintptr_t address, T& value) {
    value = {};
    return address && address <= UINTPTR_MAX - sizeof(T) && reader(address, &value, sizeof(T));
}
// Verified Road Trip placeholder depots in the loaded mod map. Match internal
// ids and empty freight definitions, never translated names such as "Roads".
inline bool RoadTripOnlyDepot(const Reader& reader,std::uintptr_t company,const std::string& id){
    if(id.size()!=5 || id.compare(0,4,"pmrt") || id[4]<'1' || id[4]>'6' || !company || company>UINTPTR_MAX-0x70)return false;
    std::uintptr_t context=0,definition=0;std::uint64_t outgoing=0,incoming=0;std::uint32_t parking=0;
    return Read(reader,company+0x68,context) && context && context<=UINTPTR_MAX-0xd8 &&
        Read(reader,company+0x10,definition) && definition && definition<=UINTPTR_MAX-0xc8 &&
        Read(reader,context+0xa8,outgoing) && !outgoing && Read(reader,context+0xd0,incoming) && !incoming &&
        Read(reader,definition+0xc0,parking) && !parking;
}
inline std::string DecodeToken(std::uint64_t token) {
    constexpr char alphabet[] = "\0" "0123456789abcdefghijklmnopqrstuvwxyz_";
    std::string text;
    while (token && text.size() < 13) {
        const auto digit = token % 38;
        if (!digit) return {};
        text += alphabet[digit]; token /= 38;
    }
    return token ? std::string{} : text;
}
inline bool LiveUnit(const Reader& reader, std::uintptr_t unit) {
    std::uint32_t flags = 0;
    return unit && unit <= UINTPTR_MAX - 0x10 && Read(reader, unit + 8, flags) && (flags & 0x80000000u);
}
// Read the same registered-unit token pool used by the native unit-name code.
// Anonymous unit names require another representation and are rejected here.
inline bool UnitName(const Reader& reader, std::uintptr_t pools, std::uintptr_t unit,
                     std::vector<std::string>& parts) {
    parts.clear();
    std::uint32_t id = 0;
    std::uintptr_t pool = 0, tokens = 0;
    std::uint8_t width = 0;
    if (!pools || pools > UINTPTR_MAX - 2048 * 8 ||
        !LiveUnit(reader, unit) || !Read(reader, unit + 0xc, id) || !id ||
        !Read(reader, pools + std::uintptr_t((id >> 20) & 0x7ff) * 8, pool) ||
        !pool || pool > UINTPTR_MAX - 0x89 || !Read(reader, pool + 0x88, width) ||
        !width || width > 16 || !Read(reader, pool, tokens)) return false;
    const auto offset = std::uintptr_t(id & 0xffff) * width * 8;
    if (!tokens || tokens > UINTPTR_MAX - offset - std::uintptr_t(width) * 8) return false;
    for (unsigned i = 0; i < width; ++i) {
        std::uint64_t token = 0;
        if (!Read(reader, tokens + offset + i * 8, token)) return false;
        auto text = DecodeToken(token);
        if (text.empty()) return false;
        parts.push_back(std::move(text));
    }
    return true;
}
inline std::string Join(const std::vector<std::string>& parts) {
    std::string id;
    for (const auto& part : parts) { if (!id.empty()) id += '.'; id += part; }
    return id;
}
inline bool PointerList(const Reader& reader, std::uintptr_t object, unsigned data_offset,
                        std::uint64_t limit, std::vector<std::uintptr_t>& values) {
    values.clear();
    std::uintptr_t data = 0;
    std::uint64_t count = 0;
    if (!object || object > UINTPTR_MAX - data_offset - 16 ||
        !Read(reader, object + data_offset, data) ||
        !Read(reader, object + data_offset + 8, count) || count > limit ||
        (count && (!data || data > UINTPTR_MAX - count * 8))) return false;
    values.resize(static_cast<std::size_t>(count));
    return !count || reader(data, values.data(), values.size() * sizeof(std::uintptr_t));
}
// Game-thread scanner: each Advance performs at most one bounded unit of work.
// Definitions and cargo identities are shared by many city instances of a base.
class CatalogScan {
public:
    bool Begin(Reader reader,std::uintptr_t economy,std::uintptr_t pools,LabelReader labels={}){
        *this=CatalogScan{};reader_=std::move(reader);labels_=std::move(labels);pools_=pools;
        if(!pools||pools>UINTPTR_MAX-2048*8||!PointerList(reader_,economy,0x28,1000000,companies_)||companies_.empty()){
            Fail("Company list is unavailable.");return false;
        }
        active_=true;return true;
    }
    bool Active() const{return active_;}
    std::size_t Done() const{return index_;}
    std::size_t Total() const{return companies_.size();}
    std::size_t CargoDefinitions() const{return cargo_cache_.size();}
    std::size_t CompanyDefinitions() const{return contexts_.size();}
    const std::string& Error() const{return error_;}
    Catalog Take(){return error_.empty()?std::move(result_):Catalog{};}
    void Advance(){
        if(!active_)return;
        if(phase_==0){
            if(index_==companies_.size()){
                if(unique_.empty()){Fail("No named companies could be read from the loaded session.");return;}
                phase_=6;return;
            }
            const auto company=companies_[index_];entry_={};
            std::uintptr_t definition=0;std::vector<std::string> parts;
            if(!seen_.insert(company).second){++index_;return;}
            if(!company||company>UINTPTR_MAX-0x70||!Read(reader_,company+0x10,definition)||!definition||
               !Read(reader_,company+0x68,context_)||!LiveUnit(reader_,context_)||
               !UnitName(reader_,pools_,company,parts)||parts.size()!=4||parts[0]!="company"||parts[1]!="volatile"){
                Skip();return;
            }
            entry_.id=Join(parts);entry_.city=parts[3];entry_.name=parts[2];entry_.city_name=entry_.city;
            if(unique_.count(entry_.id)){++index_;return;}
            if(RoadTripOnlyDepot(reader_,company,parts[2])){++result_.excluded_road_trip;++index_;return;}
            if(labels_){
                auto name=labels_(LabelKind::Company,context_),city=labels_(LabelKind::City,company);
                if(!name.empty())entry_.name=std::move(name);else ++result_.label_fallbacks;
                if(!city.empty())entry_.city_name=std::move(city);else ++result_.label_fallbacks;
                entry_.region=labels_(LabelKind::RegionId,company);
                entry_.region_name=labels_(LabelKind::Region,company);
            }
            auto cached=contexts_.find(context_);
            if(cached!=contexts_.end()){if(cached->second.valid)phase_=5;else Skip();return;}
            pending_={};pending_cargos_.clear();list_=0;phase_=1;return;
        }
        if(phase_==1){
            if(!PointerList(reader_,context_,list_==0?0xa0:0xc8,100000,pointers_)){
                pending_.valid=false;contexts_.emplace(context_,std::move(pending_));Skip();return;
            }
            // Count lists once per shared definition, not once per city base.
            if(pointers_.size()>relations_left_){Fail("Company cargo definitions exceed the scan safety limit.");return;}
            relations_left_-=pointers_.size();pointer_index_=0;list_ids_.clear();phase_=2;return;
        }
        if(phase_==2){
            if(pointer_index_==pointers_.size()){
                auto& list=list_==0?pending_.outgoing:pending_.incoming;list.assign(list_ids_.begin(),list_ids_.end());
                if(++list_<2)phase_=1;else {import_=pending_cargos_.begin();phase_=3;}return;
            }
            const auto pointer=pointers_[pointer_index_++];auto found=cargo_cache_.find(pointer);
            if(found==cargo_cache_.end()){
                if(cargo_cache_.size()>=100000){Fail("Unique cargo definitions exceed the scan safety limit.");return;}
                Cargo cargo;std::vector<std::string> parts;
                if(UnitName(reader_,pools_,pointer,parts)&&parts.size()==2&&parts[0]=="cargo"){
                    cargo.id=Join(parts);cargo.name=labels_?labels_(LabelKind::Cargo,pointer):std::string{};
                    if(cargo.name.empty()){cargo.name=parts.back();if(labels_)++result_.label_fallbacks;}
                }
                found=cargo_cache_.emplace(pointer,std::move(cargo)).first;
            }
            if(found->second.id.empty()){++result_.skipped;return;}
            list_ids_.insert(found->second.id);pending_cargos_.emplace(found->second.id,found->second);return;
        }
        if(phase_==3){
            if(import_!=pending_cargos_.end()){cargos_.insert(*import_++);return;}
            contexts_.emplace(context_,std::move(pending_));phase_=5;return;
        }
        if(phase_==5){
            const auto& data=contexts_.at(context_);
            const auto links=data.outgoing.size()+data.incoming.size();
            if(links>output_left_){Fail("Catalog company associations exceed the scan safety limit.");return;}
            output_left_-=links;entry_.outgoing=data.outgoing;entry_.incoming=data.incoming;
            unique_.emplace(entry_.id,std::move(entry_));++index_;phase_=0;return;
        }
        if(phase_==6){
            if(!unique_.empty()){auto it=unique_.begin();result_.companies.push_back(std::move(it->second));unique_.erase(it);return;}
            phase_=7;return;
        }
        if(!cargos_.empty()){auto it=cargos_.begin();result_.cargos.push_back(std::move(it->second));cargos_.erase(it);return;}
        active_=false;
    }
private:
    struct Context {bool valid=true;std::vector<std::string> outgoing,incoming;};
    Reader reader_;LabelReader labels_;std::uintptr_t pools_=0,context_=0;
    std::vector<std::uintptr_t> companies_,pointers_;
    std::set<std::uintptr_t> seen_;std::set<std::string> list_ids_;
    std::map<std::uintptr_t,Context> contexts_;std::map<std::uintptr_t,Cargo> cargo_cache_;
    std::map<std::string,Cargo> pending_cargos_,cargos_;std::map<std::string,Company> unique_;
    std::map<std::string,Cargo>::iterator import_;
    Context pending_;Company entry_;Catalog result_;std::string error_;
    std::size_t index_=0,pointer_index_=0;unsigned phase_=0,list_=0;
    std::uint64_t relations_left_=2000000,output_left_=20000000;bool active_=false;
    void Fail(const char* message){error_=message;active_=false;}
    void Skip(){++result_.skipped;++index_;phase_=0;}
};
inline bool BuildCatalog(const Reader& reader,std::uintptr_t economy,std::uintptr_t pools,
                         Catalog& catalog,std::string& error,const LabelReader& labels={}){
    CatalogScan scan;scan.Begin(reader,economy,pools,labels);
    while(scan.Active())scan.Advance();
    error=scan.Error();catalog=scan.Take();return error.empty();
}
inline bool Accepts(const Company& destination, const std::string& cargo) {
    return std::find(destination.incoming.begin(), destination.incoming.end(), cargo) != destination.incoming.end();
}
}
