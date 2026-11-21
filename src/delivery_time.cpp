#include "delivery_time.h"
#include "delivery_time_policy.h"
#include "custom_job_names.h"
#include "company_list.h"
#include <Windows.h>
#include <MinHook.h>
#include <atomic>
#include <fstream>
#include <iomanip>
#include <map>
#include <mutex>
#include <vector>
#include <cstring>
namespace delivery_time {
namespace {
using namespace custom_jobs;
using Window=void(__fastcall*)(std::uintptr_t,std::uint32_t*,std::uint32_t*,std::uintptr_t);
Window original_freight=nullptr,original_owned=nullptr;
std::uintptr_t base=0,pools=0,bound_economy=0;
std::filesystem::path file;
Logger log=nullptr;
std::atomic<bool> available{false};std::atomic<unsigned> callbacks{0};
std::mutex mutex;
std::vector<Record> records;
std::map<std::uintptr_t,Record> bindings;
std::vector<void*> hooks;
bool Memory(std::uintptr_t address,void* out,std::size_t length){if(!ReadableMemory(address,length))return false;std::memcpy(out,reinterpret_cast<void*>(address),length);return true;}
template<class T> bool Get(std::uintptr_t at,T& value){return Read(Memory,at,value);}
std::string Id(std::uintptr_t unit){std::vector<std::string> parts;return UnitName(Memory,pools,unit,parts)?Join(parts):std::string{};}
bool FingerprintOf(std::uintptr_t offer,Fingerprint& value){
    std::uintptr_t cargo=0;value={};
    if(!LiveUnit(Memory,offer)||!Get(offer+0x58,cargo)||!(value.cargo=Id(cargo)).size()||
       !Get(offer+0x48,value.expiry)||!Get(offer+0xcc,value.units)||!Get(offer+0x4c,value.urgency)||!Get(offer+0x50,value.distance))return false;
    value.destination=ReadNativeName(Memory,offer,0x10);
    return !value.destination.empty()&&value.destination.size()<=512;
}
bool Identifier(const std::string& text){return !text.empty()&&text.size()<=512&&text.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_.")==std::string::npos;}
void Load(){
    records.clear();std::error_code ec;
    if(!std::filesystem::exists(file,ec))return;
    if(std::filesystem::file_size(file,ec)>4*1024*1024||ec){if(log)log("Truck delivery-time ledger is too large or unreadable.");return;}
    std::ifstream input(file);unsigned version=0;std::size_t count=0;
    if(!(input>>version>>count)||version!=1||count>10000)return;
    std::vector<Record> loaded;
    for(std::size_t i=0;i<count;++i){Record row;unsigned owned=0,distance=0;
        if(!(input>>std::quoted(row.source)>>owned>>row.slot>>std::quoted(row.offer.cargo)>>std::quoted(row.offer.destination)>>row.offer.expiry>>row.offer.units>>row.offer.urgency>>distance)||
           owned>1||row.slot>=100000||distance>UINT16_MAX||!Identifier(row.source)||!Identifier(row.offer.cargo)||!Identifier(row.offer.destination))return;
        row.owned=owned!=0;row.offer.distance=static_cast<std::uint16_t>(distance);loaded.push_back(std::move(row));
    }
    records=std::move(loaded);
}
void Save(){
    const auto temp=std::filesystem::path(file.wstring()+L".tmp");std::ofstream output(temp,std::ios::trunc);
    output<<1<<' '<<records.size()<<'\n';
    for(const auto& row:records)output<<std::quoted(row.source)<<' '<<row.owned<<' '<<row.slot<<' '<<std::quoted(row.offer.cargo)<<' '<<std::quoted(row.offer.destination)<<' '<<row.offer.expiry<<' '<<row.offer.units<<' '<<row.offer.urgency<<' '<<row.offer.distance<<'\n';
    output.flush();const bool ok=output.good();output.close();
    if(!ok||!MoveFileExW(temp.c_str(),file.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))if(log)log("Could not persist truck delivery-time flags; current-session flags remain active.");
}
void Apply(std::uintptr_t offer,std::uint32_t* upper,std::uintptr_t economy){
    if(!available||!upper)return;
    {std::lock_guard<std::mutex> lock(mutex);if(bound_economy!=economy)return;auto found=bindings.find(offer);if(found==bindings.end())return;
        Fingerprint actual;if(!FingerprintOf(offer,actual)||!(actual==found->second.offer))return;}
    std::uint32_t now=0,extended=0;
    if(Get(economy+0x19c,now)&&Quadruple(now,*upper,extended))*upper=extended;
}
struct Callback{Callback(){++callbacks;}~Callback(){--callbacks;}};
void __fastcall FreightWindow(std::uintptr_t offer,std::uint32_t* lower,std::uint32_t* upper,std::uintptr_t economy){Callback call;original_freight(offer,lower,upper,economy);Apply(offer,upper,economy);}
void __fastcall OwnedWindow(std::uintptr_t offer,std::uint32_t* lower,std::uint32_t* upper,std::uintptr_t economy){Callback call;original_owned(offer,lower,upper,economy);Apply(offer,upper,economy);}
}
bool Available(){return available;}
bool Initialize(std::uintptr_t module,std::size_t size,const CreationCode& creation,const std::filesystem::path& directory,Logger logger){
    Shutdown();base=module;log=logger;file=directory/L"CargoControl.delivery-time.txt";
    DeliveryTimeCode code;std::uint32_t pool=0;std::string reason;
    if(!ResolveDeliveryTimeCode(reinterpret_cast<unsigned char*>(base),size,creation,code,reason)||
       !ResolveCatalogPools(reinterpret_cast<unsigned char*>(base),size,pool,reason)){
        if(log)log(("Truck delivery time x4 unavailable: "+reason).c_str());return false;
    }
    pools=base+pool;Load();
    auto add=[&](std::uint32_t rva,void* bridge,Window& original){auto target=reinterpret_cast<void*>(base+rva);
        if(MH_CreateHook(target,bridge,reinterpret_cast<void**>(&original))!=MH_OK)return false;
        hooks.push_back(target);return MH_EnableHook(target)==MH_OK;};
    if(!add(code.freight_window,reinterpret_cast<void*>(FreightWindow),original_freight)||!add(code.owned_window,reinterpret_cast<void*>(OwnedWindow),original_owned)){
        Shutdown();if(logger)logger("Truck delivery time x4 hook could not be installed.");return false;
    }
    available=true;if(log)log("Truck delivery time x4 ready: only marked custom freight/owned-trailer offers; Road Trip is unchanged.");return true;
}
void Mark(std::uintptr_t economy,std::uintptr_t company,bool owned,std::uint32_t slot,std::uintptr_t offer,bool enabled){
    if(!available)return;
    Record record;record.source=Id(company);record.owned=owned;record.slot=slot;
    if(record.source.empty())return;
    std::lock_guard<std::mutex> lock(mutex);
    for(auto it=bindings.begin();it!=bindings.end();)if(SameSlot(it->second,record))it=bindings.erase(it);else ++it;
    records.erase(std::remove_if(records.begin(),records.end(),[&](const auto& row){return SameSlot(row,record);}),records.end());bindings.erase(offer);
    if(enabled&&FingerprintOf(offer,record.offer)&&records.size()<10000){bindings[offer]=record;records.push_back(std::move(record));}
    bound_economy=economy;Save();
    if(log)log(enabled?"Created truck offer marked: delivery allowance x4; offer expiry unchanged.":"Created truck offer uses the standard delivery allowance.");
}
void Observe(std::uintptr_t economy,bool ready){
    if(!available)return;
    std::lock_guard<std::mutex> lock(mutex);
    if(!ready){bound_economy=0;bindings.clear();return;}
    if(bound_economy==economy)return;
    bindings.clear();bound_economy=economy;
    if(records.empty())return;
    std::vector<std::uintptr_t> companies;if(!PointerList(Memory,economy,0x28,1000000,companies))return;
    std::map<std::string,std::uintptr_t> sources;
    for(auto company:companies){auto id=Id(company);if(!id.empty())sources.emplace(std::move(id),company);}
    bool moved=false;
    for(auto& row:records){auto source=sources.find(row.source);if(source==sources.end())continue;
        std::vector<std::uintptr_t> offers;
        if(!PointerList(Memory,source->second,row.owned?0xa8:0x80,100000,offers))continue;
        Fingerprint actual;std::uintptr_t offer=0;std::uint32_t slot=row.slot;
        if(slot<offers.size()&&FingerprintOf(offers[slot],actual)&&actual==row.offer)offer=offers[slot];
        else for(std::uint32_t i=0;i<offers.size();++i)if(FingerprintOf(offers[i],actual)&&actual==row.offer){
            if(offer){offer=0;break;}offer=offers[i];slot=i;
        }
        if(offer){moved=moved||row.slot!=slot;row.slot=slot;bindings[offer]=row;}
    }
    if(moved)Save();
    if(log)log(("Restored truck delivery-time x4 offer flags: "+std::to_string(bindings.size())+".").c_str());
}
void Shutdown(){
    available=false;for(auto target:hooks)MH_DisableHook(target);while(callbacks.load())Sleep(1);
    for(auto target:hooks)MH_RemoveHook(target);hooks.clear();original_freight=original_owned=nullptr;
    std::lock_guard<std::mutex> lock(mutex);records.clear();bindings.clear();bound_economy=0;pools=0;log=nullptr;
}
}
