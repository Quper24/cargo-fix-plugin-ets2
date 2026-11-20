#include "custom_job_creation.h"
#include "owned_offers.h"
#include "custom_job_events.h"
#include <MinHook.h>
#include <Windows.h>
#include <atomic>
#include <cstring>

namespace custom_jobs::creation {
namespace {
using Vector=GameOfferVector;
using OwnedCandidates=std::uint64_t(__fastcall*)(std::uintptr_t,Vector*,std::uintptr_t,std::uintptr_t);
using BaseCandidates=void(__fastcall*)(std::uintptr_t,Vector*,std::uintptr_t,float,std::uintptr_t);
using Destinations=std::uint64_t(__fastcall*)(std::uintptr_t,Vector*,std::uintptr_t,std::uintptr_t,std::uintptr_t,const std::uint64_t*,float,std::uintptr_t);
using Schedule=void(__fastcall*)(std::uintptr_t,std::uintptr_t,std::uint32_t,std::uintptr_t,std::uint32_t);
using Physical=void(__fastcall*)(std::uintptr_t,std::uintptr_t,std::uint64_t);
OwnedCandidates original_owned=nullptr;
BaseCandidates original_base=nullptr;
Destinations original_destinations=nullptr;
Schedule original_schedule=nullptr;
Physical original_physical=nullptr;
std::uintptr_t module=0;
GameProfile core{};
CreationCode code{};
Backend backend{};
Logger logger=nullptr;
bool initialized=false;
std::vector<void*> hooks;
std::atomic<unsigned> callbacks{0};
struct Callback {Callback(){++callbacks;}~Callback(){--callbacks;}};
struct Selection {
    Market market=Market::Freight;
    std::uintptr_t origin=0,destination=0,cargo=0,definition=0;
    std::uint32_t slot=0;
    bool physical=false,scheduled=false;
};
thread_local Selection* selected=nullptr;
struct SelectionScope {Selection* previous;explicit SelectionScope(Selection& value):previous(selected){selected=&value;}~SelectionScope(){selected=previous;}};
bool Memory(std::uintptr_t address,void* out,std::size_t length){
    if(!ReadableMemory(address,length))return false;std::memcpy(out,reinterpret_cast<const void*>(address),length);return true;
}
template<class T> bool Get(std::uintptr_t address,T& value){return Read(Memory,address,value);}
bool Writable(std::uintptr_t address,std::size_t length){
    if(!address || !length || address>UINTPTR_MAX-length)return false;
    const auto end=address+length;
    while(address<end){
        MEMORY_BASIC_INFORMATION region{};
        if(!VirtualQuery(reinterpret_cast<const void*>(address),&region,sizeof(region)) || region.State!=MEM_COMMIT || (region.Protect&PAGE_GUARD))return false;
        const auto p=region.Protect&0xff;
        if(p!=PAGE_READWRITE && p!=PAGE_WRITECOPY && p!=PAGE_EXECUTE_READWRITE && p!=PAGE_EXECUTE_WRITECOPY)return false;
        const auto start=reinterpret_cast<std::uintptr_t>(region.BaseAddress);
        if(start>UINTPTR_MAX-region.RegionSize || start+region.RegionSize<=address)return false;
        address=start+region.RegionSize;
    }
    return true;
}
template<class T> void Put(std::uintptr_t address,const T& value){std::memcpy(reinterpret_cast<void*>(address),&value,sizeof(value));}
void FilterCargo(Vector* values){
    if(!values || !ValidOfferVector(*values)){if(values)values->count=0;return;}
    values->count=KeepCandidate(values->data,values->count,selected->cargo);
}
std::uint64_t __fastcall OwnedBridge(std::uintptr_t company,Vector* values,std::uintptr_t context,std::uintptr_t trailer){
    Callback guard;const auto result=original_owned(company,values,context,trailer);
    if(selected && selected->market==Market::OwnedTrailer && selected->origin==company){FilterCargo(values);return values?values->count:0;}
    return result;
}
void __fastcall BaseBridge(std::uintptr_t company,Vector* values,std::uintptr_t context,float limit,std::uintptr_t economy){
    Callback guard;original_base(company,values,context,limit,economy);
    if(selected && selected->market==Market::Freight && selected->origin==company)FilterCargo(values);
}
std::uint64_t FilterDestinations(Vector* values,std::uint64_t cutoff,std::uintptr_t destination){
    if(!values || !ValidOfferVector(*values)){if(values)values->count=0;return 0;}
    return KeepDestination(Memory,values->data,values->count,cutoff,destination)?UINT64_MAX:0;
}
std::uint64_t __fastcall DestinationBridge(std::uintptr_t company,Vector* values,std::uintptr_t unused,std::uintptr_t context,
                                          std::uintptr_t cargo,const std::uint64_t* filter,float limit,std::uintptr_t trailer){
    Callback guard;const auto result=original_destinations(company,values,unused,context,cargo,filter,limit,trailer);
    if(selected && selected->origin==company && selected->cargo==cargo)return FilterDestinations(values,result,selected->destination);
    return result;
}
void __fastcall ScheduleBridge(std::uintptr_t scheduler,std::uintptr_t economy,std::uint32_t time,std::uintptr_t company,std::uint32_t slot){
    Callback guard;
    if(selected && selected->market==Market::Freight && selected->origin==company && selected->slot==slot){selected->scheduled=true;return;}
    original_schedule(scheduler,economy,time,company,slot);
}
void __fastcall PhysicalBridge(std::uintptr_t definition,std::uintptr_t economy,std::uint64_t slot){
    Callback guard;
    if(selected && selected->market==Market::Freight && selected->definition==definition && selected->slot==slot){selected->physical=true;return;}
    original_physical(definition,economy,slot);
}
template<class Function> bool AddHook(std::uint32_t rva,void* bridge,Function& original){
    if(!rva)return false;
    auto* target=reinterpret_cast<void*>(module+rva);
    if(MH_CreateHook(target,bridge,reinterpret_cast<void**>(&original))!=MH_OK)return false;
    hooks.push_back(target);return true;
}
void Release(std::uintptr_t& offer,std::uintptr_t vtable){
    if(!offer)return;
    Vector one{vtable,&offer,1,1};
    reinterpret_cast<void(__fastcall*)(Vector*)>(module+core.clear_vector)(&one);
    offer=0;
}
struct NativeOffer {
    std::uintptr_t value=0,vtable=0;
    ~NativeOffer(){Release(value,vtable);}
};
struct Scratch {
    std::vector<std::uintptr_t> storage;
    Vector values;
    Scratch(std::uintptr_t vtable,std::size_t count):storage(std::max<std::size_t>(1,count)),values{vtable,storage.data(),0,storage.size()}{ }
};
struct SlotSwap {
    std::uintptr_t address=0,old=0;
    bool committed=false;
    SlotSwap(std::uintptr_t slot,std::uintptr_t value):address(slot){Get(slot,old);Put(slot,value);}
    ~SlotSwap(){if(!committed)Put(address,old);}
};
struct OwnedSwap {
    Vector* address;
    Vector previous;
    std::uintptr_t special_address=0,special=0;
    std::vector<std::uint32_t> timers;
    std::uint32_t* timer_data=nullptr;
    bool restored=false;
    OwnedSwap(Vector* target,Vector replacement,std::uintptr_t company,std::uint32_t* data,std::uint64_t count)
        :address(target),previous(*target),special_address(company+0x108),timers(data,data+count),timer_data(data){
        Get(special_address,special);Put<std::uintptr_t>(special_address,0);*address=replacement;
    }
    void Restore(){if(restored)return;*address=previous;Put(special_address,special);std::copy(timers.begin(),timers.end(),timer_data);restored=true;}
    ~OwnedSwap(){Restore();}
};
bool Events(std::uintptr_t scheduler,std::uintptr_t company,std::uint32_t slot,std::vector<EventNode>& result){
    return CollectSlotEvents(Memory,Writable,scheduler,company,slot,result);
}
void RemoveEvent(std::uintptr_t scheduler,std::uintptr_t node){
    if(UnlinkSlotEvent(Memory,Writable,[](std::uintptr_t at,const void* data,std::size_t length){
        std::memcpy(reinterpret_cast<void*>(at),data,length);
    },scheduler,node))
        reinterpret_cast<void(__fastcall*)(std::uintptr_t,std::uintptr_t)>(module+code.free_event_node)(scheduler+0x38,node);
}
bool Matches(std::uintptr_t offer,std::uintptr_t cargo,const std::string& target,std::uint32_t now){
    OfferIdentity id;std::uint32_t expires=0,units=0;
    return ReadOfferIdentity(offer,id) && id.cargo==cargo && id.destination==target &&
        Get(offer+0x48,expires) && expires>now && Get(offer+0xcc,units) && units;
}
bool LiveSelection(std::uintptr_t economy,std::uintptr_t pools,const JobRequest& request,Selection& result,std::string& target){
    std::vector<std::uintptr_t> companies,cargos,accepts;
    std::vector<std::string> parts;
    if(!PointerList(Memory,economy,0x28,1000000,companies))return false;
    for(const auto company:companies){
        if(!UnitName(Memory,pools,company,parts))continue;
        const auto id=Join(parts);
        if(id==request.origin)result.origin=company;
        if(id==request.destination)result.destination=company;
        if(result.origin && result.destination)break;
    }
    std::uintptr_t context=0,destination_context=0;
    if(!result.origin || !result.destination || result.origin==result.destination ||
       !Get(result.origin+0x68,context)||!LiveUnit(Memory,context)||!Get(result.destination+0x68,destination_context)||!LiveUnit(Memory,destination_context)||
       !Get(result.origin+0x10,result.definition)||!result.definition ||
       !PointerList(Memory,context,0xa0,100000,cargos)||!PointerList(Memory,destination_context,0xc8,100000,accepts))return false;
    for(const auto cargo:cargos)if(UnitName(Memory,pools,cargo,parts)&&Join(parts)==request.cargo){result.cargo=cargo;break;}
    std::uint64_t prefix=0;
    if(!result.cargo || std::find(accepts.begin(),accepts.end(),result.cargo)==accepts.end() ||
       !Get(module+code.destination_prefix,prefix) || prefix>=request.destination.size())return false;
    target=request.destination.substr(static_cast<std::size_t>(prefix));result.market=request.market;
    return Writable(result.origin+0x78,sizeof(Vector)) && Writable(result.origin+0xa0,sizeof(Vector));
}
std::uintptr_t TrailerData(std::uintptr_t economy){
    std::uintptr_t player=0,refs=0,owner=0,trailer=0,data=0;std::uint64_t count=0;
    if(!Get(economy+0x18,player)||!Get(player+0x80,count)||!count||!Get(player+0x78,refs)||!Get(refs,owner)||!LiveUnit(Memory,owner)||
       !Get(owner+0x48,trailer)||!LiveUnit(Memory,trailer)||!Get(trailer+0x1a0,data)||!LiveUnit(Memory,data)||!ReadableMemory(data,0x50))return 0;
    return data;
}
bool PhysicalSlot(std::uintptr_t definition,std::uint32_t slot){
    std::uint32_t flags=0,configured=0;std::uint64_t nodes=0,descriptors=0;std::uintptr_t array=0;
    if(!Get(definition+0xc4,flags))return false;
    if(!(flags&2))return true;
    return Get(definition+0xc0,configured)&&slot<configured&&Get(definition+0x78,nodes)&&configured<=nodes&&slot<nodes&&
        Get(definition+0xe0,descriptors)&&descriptors<=nodes&&slot<descriptors&&Get(definition+0xd8,array)&&ReadableMemory(array+slot*8,8);
}
CreateStatus Owned(std::uintptr_t economy,Selection& selection,const std::string& target,std::uint32_t now,std::uint32_t deadline){
    if(!backend.prepare || !backend.generate)return CreateStatus::Unsupported;
    backend.prepare(selection.origin,economy);
    auto* offers=reinterpret_cast<Vector*>(selection.origin+0xa0);
    if(!ValidOfferVector(*offers))return CreateStatus::InvalidGameData;
    for(std::uint64_t n=0;n<offers->count;++n)if(Matches(offers->data[n],selection.cargo,target,now))return CreateStatus::AlreadyPresent;
    reinterpret_cast<void(__fastcall*)(std::uintptr_t,std::uintptr_t)>(module+core.refill)(selection.origin,economy);
    std::uint64_t count=0;std::uintptr_t timers=0;
    if(!Get(selection.origin+0x50,count)||!count||count>10000||!Get(selection.origin+0x48,timers)||!Writable(timers,count*4)||!Writable(selection.origin+0x108,8))return CreateStatus::NoSlot;
    NativeOffer created;created.vtable=offers->vtable;
    Vector generated{offers->vtable,&created.value,0,1};
    OwnedSwap temporary(offers,generated,selection.origin,reinterpret_cast<std::uint32_t*>(timers),count);
    {SelectionScope scope(selection);backend.generate(selection.origin,economy);}
    const auto generated_count=offers->count;
    temporary.Restore();
    if(generated_count!=1 || !Matches(created.value,selection.cargo,target,now))return CreateStatus::GenerationFailed;
    Put(created.value+0x48,deadline);Put(created.value+0x60,deadline);
    if(offers->count>=100000 || !Writable(reinterpret_cast<std::uintptr_t>(offers),sizeof(Vector)))return CreateStatus::NoSlot;
    if(offers->count==offers->capacity){
        using Reserve=bool(__fastcall*)(Vector*,std::uint64_t);
        std::uintptr_t reserve=0;
        if(!Get(offers->vtable+24,reserve)||!ReadableMemory(reserve,1)||!reinterpret_cast<Reserve>(reserve)(offers,offers->count+1))return CreateStatus::AllocationFailed;
    }
    if(!ValidOfferVector(*offers)||offers->count>=offers->capacity||!Writable(reinterpret_cast<std::uintptr_t>(offers->data+offers->count),8))return CreateStatus::AllocationFailed;
    offers->data[offers->count++]=created.value;created.value=0;
    return CreateStatus::Created;
}
CreateStatus Freight(std::uintptr_t economy,Selection& selection,const std::string& target,std::uint32_t now,std::uint32_t deadline,bool replace){
    auto* offers=reinterpret_cast<Vector*>(selection.origin+0x78);
    if(!ValidOfferVector(*offers)||!offers->count)return CreateStatus::NoSlot;
    std::vector<OfferState> states;
    for(std::uint64_t n=0;n<offers->count;++n){
        OfferState state;const auto offer=offers->data[n];
        if(offer){
            if(!Get(offer+0x58,state.cargo)||!Get(offer+0x48,state.expires))return CreateStatus::InvalidGameData;
            if(state.cargo){OfferIdentity identity;if(!ReadOfferIdentity(offer,identity))return CreateStatus::InvalidGameData;state.destination=identity.destination;}
        }
        states.push_back(std::move(state));
    }
    const auto choice=ChooseFreightSlot(states,selection.cargo,target,now,replace);
    if(choice.status!=CreateStatus::Created)return choice.status;
    selection.slot=static_cast<std::uint32_t>(choice.index);
    const auto address=reinterpret_cast<std::uintptr_t>(offers->data+choice.index);
    std::uintptr_t scheduler=0;
    std::vector<EventNode> old_events;
    std::uint32_t scheduler_now=0;
    if(!Writable(address,8)||!PhysicalSlot(selection.definition,selection.slot)||!Get(economy+0x168,scheduler)||!Get(scheduler+0xa8,scheduler_now)||
       deadline<=scheduler_now||!Events(scheduler,selection.origin,selection.slot,old_events))return CreateStatus::InvalidGameData;
    std::vector<EventNode> current,after;
    current.reserve(old_events.size());after.reserve(old_events.size()+1);
    std::uint32_t allocator_index=0;std::uintptr_t allocator=0;
    if(!Get(module+code.allocator_index,allocator_index)||allocator_index>=4||!Get(module+code.allocator_array+allocator_index*8,allocator)||!ReadableMemory(allocator,0x18))return CreateStatus::InvalidGameData;
    NativeOffer created;created.vtable=offers->vtable;
    created.value=reinterpret_cast<std::uintptr_t(__fastcall*)(std::uintptr_t)>(module+code.base_factory)(allocator);
    if(!created.value)return CreateStatus::AllocationFailed;
    // Match native fill's adoption of a newly allocated reference-counted unit.
    InterlockedExchangeAdd(reinterpret_cast<volatile LONG*>(created.value+8),0x20000001);
    InterlockedXor(reinterpret_cast<volatile LONG*>(created.value+8),0x60000000);
    InterlockedXor(reinterpret_cast<volatile LONG*>(created.value+8),0x60000000);
    SlotSwap temporary(address,created.value);
    {SelectionScope scope(selection);
        reinterpret_cast<void(__fastcall*)(std::uintptr_t,std::uintptr_t,std::uint32_t,std::uint64_t,bool)>(module+code.base_slot)(selection.origin,economy,now,selection.slot,false);
    }
    if(!selection.scheduled || !Matches(created.value,selection.cargo,target,now))return CreateStatus::GenerationFailed;
    Put(created.value+0x48,deadline);Put(created.value+0x60,deadline);
    if(!Events(scheduler,selection.origin,selection.slot,current))return CreateStatus::InvalidGameData;
    // Register a replacement expiry first. On allocation failure old events survive.
    original_schedule(scheduler,economy,deadline,selection.origin,selection.slot);
    if(!Events(scheduler,selection.origin,selection.slot,after))return CreateStatus::SchedulingFailed;
    auto previous=[&](std::uintptr_t node){return std::any_of(current.begin(),current.end(),[&](const auto& event){return event.node==node;});};
    const bool registered=std::any_of(after.begin(),after.end(),[&](const auto& event){return !previous(event.node)&&event.time==deadline;});
    if(!registered){for(const auto& event:after)if(!previous(event.node))RemoveEvent(scheduler,event.node);return CreateStatus::SchedulingFailed;}
    for(const auto& event:current)RemoveEvent(scheduler,event.node);
    temporary.committed=true;created.value=0;
    if(selection.physical)original_physical(selection.definition,economy,selection.slot);
    Release(temporary.old,offers->vtable);
    return CreateStatus::Created;
}
}

bool Initialize(std::uintptr_t base,const GameProfile& profile,const CreationCode& selected_code,Backend owned,Logger log){
    Shutdown();module=base;core=profile;code=selected_code;backend=owned;logger=log;
    if(!code.base_slot)return false;
    if(!AddHook(code.owned_candidates,reinterpret_cast<void*>(OwnedBridge),original_owned)||
       !AddHook(code.base_candidates,reinterpret_cast<void*>(BaseBridge),original_base)||
       !AddHook(code.destinations,reinterpret_cast<void*>(DestinationBridge),original_destinations)||
       !AddHook(code.schedule,reinterpret_cast<void*>(ScheduleBridge),original_schedule)||
       !AddHook(code.physical_update,reinterpret_cast<void*>(PhysicalBridge),original_physical)){Shutdown();return false;}
    for(auto* target:hooks)if(MH_EnableHook(target)!=MH_OK){Shutdown();return false;}
    initialized=true;if(logger)logger("Targeted native job creation enabled; candidate filters are scoped to one request/thread.");return true;
}
void Shutdown(){
    initialized=false;
    for(auto* target:hooks)MH_DisableHook(target);
    while(callbacks.load())Sleep(1);
    for(auto* target:hooks)MH_RemoveHook(target);
    hooks.clear();original_owned=nullptr;original_base=nullptr;original_destinations=nullptr;original_schedule=nullptr;original_physical=nullptr;
    module=0;core={};code={};backend={};logger=nullptr;
}
CreateResult Execute(std::uintptr_t economy,std::uintptr_t pools,const JobRequest& request){
    auto result=[&](CreateStatus status){return CreateResult{status,request.market};};
    if(!initialized)return result(CreateStatus::Unsupported);
    if(selected || !economy || !pools)return result(CreateStatus::NotReady);
    Selection selection;std::string target;
    if(!LiveSelection(economy,pools,request,selection,target))return result(CreateStatus::InvalidSelection);
    if(logger)logger(("Targeted native request: source="+request.origin+", cargo="+request.cargo+", destination="+request.destination+
        ", market="+std::to_string(static_cast<int>(request.market))+".").c_str());
    std::uint32_t now=0;
    if(!Get(economy+0x19c,now)||now>UINT32_MAX-48u*60u)return result(CreateStatus::InvalidGameData);
    const auto deadline=now+48u*60u;
    std::uintptr_t context=0;
    if(!Get(economy+0x7b8,context)||!ReadableMemory(context,0x20))return result(CreateStatus::InvalidGameData);
    const auto* offer_vector=reinterpret_cast<const Vector*>(selection.origin+0xa0);
    if(!ReadableMemory(reinterpret_cast<std::uintptr_t>(offer_vector),sizeof(Vector))||!ValidOfferVector(*offer_vector))return result(CreateStatus::InvalidGameData);
    std::uintptr_t trailer=0;
    if(request.market==Market::OwnedTrailer){
        trailer=TrailerData(economy);if(!trailer)return result(CreateStatus::TrailerRequired);
        std::uintptr_t source_context=0;std::uint64_t count=0;
        if(!Get(selection.origin+0x68,source_context)||!Get(source_context+0xa8,count)||count>100000)return result(CreateStatus::InvalidGameData);
        Scratch cargo_candidates(offer_vector->vtable,static_cast<std::size_t>(count));
        original_owned(selection.origin,&cargo_candidates.values,context,trailer);
        if(!ValidOfferVector(cargo_candidates.values)||!KeepCandidate(cargo_candidates.values.data,cargo_candidates.values.count,selection.cargo))return result(CreateStatus::CargoUnavailable);
    }
    std::uintptr_t routes=0;std::uint64_t route_count=0;
    if(!Get(selection.origin+0x20,routes)||!Get(selection.origin+0x28,route_count)||route_count>1000000 ||
        (route_count && !ReadableMemory(routes,route_count*16)))return result(CreateStatus::InvalidGameData);
    Scratch destinations(offer_vector->vtable,static_cast<std::size_t>(route_count));
    const auto cutoff=original_destinations(selection.origin,&destinations.values,0,context,selection.cargo,nullptr,0.f,trailer);
    if(FilterDestinations(&destinations.values,cutoff,selection.destination)!=UINT64_MAX)return result(CreateStatus::DestinationUnavailable);
    const auto status=request.market==Market::OwnedTrailer?Owned(economy,selection,target,now,deadline):Freight(economy,selection,target,now,deadline,request.allow_replace);
    if(logger){const auto message="Targeted creation result="+std::to_string(static_cast<int>(status))+", origin="+request.origin+", cargo="+request.cargo+", destination="+request.destination+", market="+std::to_string(static_cast<int>(request.market))+".";logger(message.c_str());}
    return result(status);
}
}
