#include "custom_job_request.h"
#include "custom_job_events.h"
#include "custom_trailer_cargos.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#define Check(value) do {if(!(value)){std::fprintf(stderr,"Creation check failed: %d\n",__LINE__);std::exit(1);}}while(0)
using namespace custom_jobs;
struct List {
    std::vector<unsigned char> bytes=std::vector<unsigned char>(4096);
    unsigned writes=0;
    bool Range(std::uintptr_t at,std::size_t count){return at>=0x100 && at<bytes.size() && count<=bytes.size()-at;}
    Reader read=[this](std::uintptr_t at,void* out,std::size_t size){if(!Range(at,size))return false;std::memcpy(out,bytes.data()+at,size);return true;};
    creation::WritableCheck writable=[this](std::uintptr_t at,std::size_t size){return Range(at,size);};
    creation::Writer write=[this](std::uintptr_t at,const void* data,std::size_t size){Check(Range(at,size));++writes;std::memcpy(bytes.data()+at,data,size);};
    template<class T> void Put(std::uintptr_t at,T value){Check(Range(at,sizeof(value)));std::memcpy(bytes.data()+at,&value,sizeof(value));}
    template<class T> T Get(std::uintptr_t at){T value{};Check(Read(read,at,value));return value;}
    List(){
        Put<std::uint64_t>(0x140,4);
        const std::uintptr_t nodes[]={0x148,0x200,0x240,0x280,0x2c0};
        for(unsigned n=0;n<5;++n){Put(nodes[n],nodes[(n+1)%5]);Put(nodes[n]+8,nodes[(n+4)%5]);}
        for(unsigned n=1;n<5;++n){const std::uintptr_t event=0x400+n*0x40;Put(nodes[n]+16,event);Put<std::uintptr_t>(event+0x18,n==3?0xbbb:0xaaa);Put<std::uint32_t>(event+0x20,n==2?1:0);Put<std::uint32_t>(event+0x10,100+n);}
    }
};
int main(){
    Catalog catalog;
    catalog.companies={{"company.volatile.a.x","x","Coca-Cola",{"cargo.salt"},{}},
                       {"company.volatile.b.y","y","FedEx",{},{"cargo.salt"}},
                       {"company.volatile.c.y","y","FedEx",{},{"cargo.other"}}};
    catalog.cargos={{"cargo.salt","Salt"},{"cargo.mod_tank","Mod cargo"}};
    catalog.companies[1].incoming.push_back("cargo.mod_tank");
    JobRequest request{7,Market::Freight,"company.volatile.a.x","cargo.salt","company.volatile.b.y",false};
    Check(ValidSelection(catalog,request));
    auto wrong=request;wrong.destination="FedEx";Check(!ValidSelection(catalog,wrong));
    wrong=request;wrong.destination="company.volatile.c.y";Check(!ValidSelection(catalog,wrong));
    wrong=request;wrong.destination=wrong.origin;Check(!ValidSelection(catalog,wrong));
    wrong=request;wrong.cargo="Salt";Check(!ValidSelection(catalog,wrong));
    request.market=Market::OwnedTrailer;Check(ValidSelection(catalog,request));
    auto foreign=request;foreign.cargo="cargo.mod_tank";
    Check(ValidSelection(catalog,foreign)); // Owned cargo is independent of the source's outgoing list.
    foreign.market=Market::Freight;Check(!ValidSelection(catalog,foreign));
    foreign.all_cargos=true;Check(ValidSelection(catalog,foreign));
    foreign.destination="company.volatile.c.y";Check(ValidSelection(catalog,foreign)); // Receiver assortment is optional too.
    foreign.all_cargos=false;Check(!ValidSelection(catalog,foreign));
    foreign.all_cargos=true;foreign.cargo="cargo.not_loaded";Check(!ValidSelection(catalog,foreign));
    foreign.cargo="cargo.mod_tank";foreign.destination=foreign.origin;Check(!ValidSelection(catalog,foreign));
    TrailerCargos trailer;
    Check(AvailableCargos(catalog,&catalog.companies[0],false,trailer).size()==1);
    Check(AvailableCargos(catalog,&catalog.companies[0],true,trailer).empty());
    Check(AvailableCargos(catalog,&catalog.companies[0],false,trailer,true).size()==2);
    Check(AvailableCargos(catalog,nullptr,false,trailer,true).size()==2);
    Check(AvailableCargos(catalog,&catalog.companies[0],true,trailer,true).empty()); // No trailer still means no compatible cargo.
    trailer.status=TrailerCargoStatus::Ready;
    trailer.cargos={"cargo.salt","cargo.mod_tank"};
    const auto global=AvailableCargos(catalog,&catalog.companies[0],true,trailer);
    Check(global.size()==2);
    Check(global==AvailableCargos(catalog,&catalog.companies[1],true,trailer));
    Check(global==AvailableCargos(catalog,nullptr,true,trailer));
    trailer.cargos={"cargo.mod_tank"};
    Check(!trailer.Allows(request.cargo));
    Check(AvailableCargos(catalog,&catalog.companies[0],true,trailer)==std::vector<std::string>{"cargo.mod_tank"});
    trailer.status=TrailerCargoStatus::NoTrailer;
    Check(AvailableCargos(catalog,&catalog.companies[0],true,trailer).empty());
    Check(AvailableCargos(catalog,&catalog.companies[0],false,trailer).size()==1);
    trailer.status=TrailerCargoStatus::Failed;Check(!trailer.Allows("cargo.mod_tank"));
    auto any_receiver=request;any_receiver.destination="company.volatile.c.y";
    Check(ValidSelection(catalog,any_receiver)); // Direct owned jobs ignore receiver assortment too.
    any_receiver.market=Market::Freight;Check(!ValidSelection(catalog,any_receiver));
    RequestQueue queue;Check(queue.Push(request));Check(!queue.Push(wrong));
    Check(queue.Take()->destination==request.destination);Check(!queue.Take());
    Check(queue.Push(request));queue.Clear();Check(!queue.Take());
    std::vector<OfferState> offers={{1,"b.y",200},{2,"c.z",200},{0,"",0}};
    Check(ChooseFreightSlot(offers,1,"b.y",100,false).status==CreateStatus::AlreadyPresent);
    Check(ChooseFreightSlot(offers,1,"d.z",100,false).status==CreateStatus::ReplaceRequired);
    Check(ChooseFreightSlot(offers,1,"d.z",100,true).index==0);
    Check(ChooseFreightSlot(offers,3,"d.z",100,false).index==2);
    offers[0].expires=100;Check(ChooseFreightSlot(offers,3,"d.z",100,false).index==0);
    offers.resize(2);offers[0].expires=200;
    Check(ChooseFreightSlot(offers,3,"d.z",100,false).status==CreateStatus::ReplaceRequired);
    Check(ChooseFreightSlot(offers,3,"d.z",100,true).status==CreateStatus::Created);
    Check(ChooseFreightSlot({},3,"d.z",100,true).status==CreateStatus::NoSlot);
    std::uintptr_t candidates[]={11,22,33};Check(KeepCandidate(candidates,3,22)==1 && candidates[0]==22);
    Check(KeepCandidate(candidates,3,44)==0);
    using namespace custom_jobs::creation;
    List list;std::vector<EventNode> selected;
    list.Put<std::uintptr_t>(0x720,0x800);list.Put<std::uint64_t>(0x728,2);
    const DirectRouteRow first{0xaaa,150,3,0x1234},second{0xbbb,250,UINT16_MAX,0x4321};
    list.Put(0x800,first);list.Put(0x810,second);
    DirectRouteRow route;
    Check(ReadDirectRoute(list.read,0x700,0xbbb,route));
    Check(route.destination==0xbbb && route.distance==250 && route.ferry==UINT16_MAX && route.reserved==0x4321);
    Check(!ReadDirectRoute(list.read,0x700,0xccc,route));
    bool approximate=false;
    Check(PrepareDirectRoute(list.read,0x700,0xbbb,route,approximate));
    Check(!approximate && route.distance==250 && route.reserved==0x4321);
    // Missing forward data uses only the reverse distance, never its ferry index.
    list.Put<std::uintptr_t>(0xcec,0xd00);list.Put<std::uint64_t>(0xcf4,1);
    list.Put(0xd00,DirectRouteRow{0x700,420,7,0x5678});
    Check(PrepareDirectRoute(list.read,0x700,0xccc,route,approximate));
    Check(approximate && route.destination==0xccc && route.distance==420 && route.ferry==UINT16_MAX && route.reserved==0);
    list.Put<std::uint64_t>(0xcf4,0);
    Check(PrepareDirectRoute(list.read,0x700,0xccc,route,approximate));
    Check(approximate && route.distance==250); // Upper median of existing positive distances.
    list.Put<std::uint64_t>(0x728,0);list.Put<std::uintptr_t>(0x720,0);
    Check(PrepareDirectRoute(list.read,0x700,0xccc,route,approximate));
    Check(approximate && route.distance==1000 && route.destination==0xccc);
    list.Put<std::uint64_t>(0x728,1); // A nonempty table with a null pointer stays an error.
    Check(!PrepareDirectRoute(list.read,0x700,0xccc,route,approximate));
    Check(!approximate);
    list.Put<std::uintptr_t>(0x720,0xff8);
    Check(!PrepareDirectRoute(list.read,0x700,0xccc,route,approximate));
    list.Put<std::uintptr_t>(0x720,0x800);list.Put<std::uint64_t>(0x728,2);
    Check(!ReadDirectRoute(list.read,UINTPTR_MAX-8,0xbbb,route));
    list.Put<std::uint64_t>(0x728,1000001);Check(!ReadDirectRoute(list.read,0x700,0xbbb,route));
    Check(!PrepareDirectRoute(list.read,0x700,0xccc,route,approximate));
    // Trailer countries and cargo frequency/skills do not affect physical compatibility.
    list.Put<std::uintptr_t>(0x9a8,0xb00);list.Put<std::uint64_t>(0x9b0,1);list.Put<std::uint64_t>(0xb00,17);
    list.Put<std::uint64_t>(0xa48,17);list.Put<float>(0xa40,100);list.Put<float>(0xa30,1000);
    list.Put<float>(0x998,10);list.Put<float>(0x99c,20);
    Check(CargoFitsTrailer(list.read,0x900,0xa00));
    list.Put<std::uint64_t>(0xa48,18);Check(!CargoFitsTrailer(list.read,0x900,0xa00));
    list.Put<std::uint64_t>(0xa48,17);list.Put<float>(0xa40,5);Check(!CargoFitsTrailer(list.read,0x900,0xa00));
    list.Put<float>(0xa40,100);list.Put<float>(0xa30,10);Check(!CargoFitsTrailer(list.read,0x900,0xa00));
    Check(!CargoFitsTrailer(list.read,UINTPTR_MAX-8,0xa00));
    list.Put<std::uintptr_t>(0x600,0xaaa);list.Put<std::uintptr_t>(0x610,0xbbb);
    list.Put<std::uint32_t>(0x618,0x12345678);
    std::uintptr_t rows[]={0x600,0x610};std::uint64_t row_count=2;
    Check(!KeepDestination(list.read,rows,row_count,1,0xbbb)&&row_count==0);
    row_count=2;Check(KeepDestination(list.read,rows,row_count,UINT64_MAX,0xbbb)&&row_count==1&&rows[0]==0x610);
    Check(list.Get<std::uint32_t>(rows[0]+8)==0x12345678); // Borrowed route metadata stays intact.
    Check(CollectSlotEvents(list.read,list.writable,0x100,0xaaa,0,selected));
    Check(selected.size()==2 && selected[0].node==0x200 && selected[1].node==0x2c0);
    for(const auto& event:selected)Check(UnlinkSlotEvent(list.read,list.writable,list.write,0x100,event.node));
    Check(list.Get<std::uint64_t>(0x140)==2);
    Check(CollectSlotEvents(list.read,list.writable,0x100,0xaaa,0,selected)&&selected.empty());
    Check(CollectSlotEvents(list.read,list.writable,0x100,0xaaa,1,selected)&&selected.size()==1&&selected[0].node==0x240);
    Check(CollectSlotEvents(list.read,list.writable,0x100,0xbbb,0,selected)&&selected.size()==1&&selected[0].node==0x280);
    Check(!UnlinkSlotEvent(list.read,list.writable,list.write,0x100,0x148));
    List corrupt;corrupt.Put<std::uintptr_t>(0x208,0x999);
    Check(!CollectSlotEvents(corrupt.read,corrupt.writable,0x100,0xaaa,0,selected));
    Check(!UnlinkSlotEvent(corrupt.read,corrupt.writable,corrupt.write,0x100,0x200)&&corrupt.writes==0);
    List cycle;cycle.Put<std::uintptr_t>(0x2c0,0x200);
    Check(!CollectSlotEvents(cycle.read,cycle.writable,0x100,0xaaa,0,selected));
    List count;count.Put<std::uint64_t>(0x140,3);
    Check(!CollectSlotEvents(count.read,count.writable,0x100,0xaaa,0,selected));
    std::puts("Creation selection, queue, replacement and targeted event checks passed.");
}
