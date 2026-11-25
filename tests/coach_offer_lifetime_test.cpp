#include "coach_offer_lifetime.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <set>

#define Check(value) do{if(!(value)){std::fprintf(stderr,"Coach lifetime check failed: %d\n",__LINE__);std::exit(1);}}while(0)
struct Station {
    std::array<std::uintptr_t,16> storage{};
    GameOfferVector offers{123,storage.data(),10,16};
    std::set<std::uintptr_t> serialized,events;
    std::uintptr_t selected=0;
    Station(){for(unsigned i=0;i<10;++i){storage[i]=100+i;serialized.insert(storage[i]);events.insert(i);}}
    void Clear(){
        // Matches native clear: destruction unregisters each unit independently
        // of references retained by somebody else. Memory addresses may survive.
        for(unsigned i=0;i<offers.count;++i){if(selected==offers.data[i])selected=0;serialized.erase(offers.data[i]);}
        offers.count=0;events.clear();
    }
    void Schedule(){for(unsigned i=0;i<offers.count;++i)events.insert(i);}
    void Reload(){for(unsigned i=0;i<offers.count;++i)Check(serialized.count(offers.data[i])==1);Check(events.size()==offers.count);}
};
int main(){
    for(unsigned slot:{0u,5u,9u}){
        Station station;station.selected=100+slot;station.serialized.insert(999);
        coaches::ReplaceOffer(station.offers,slot,999,[&]{station.Clear();});station.Schedule();
        Check(station.offers.data==station.storage.data() && station.offers.count==10 && station.offers.capacity==16);
        Check(station.selected==0 && !station.serialized.count(100+slot));station.Reload();
    }
    {Station station;station.selected=105;station.serialized.insert(999);
        coaches::ReplaceOffer(station.offers,0,999,[&]{station.Clear();});station.Schedule();
        Check(station.selected==105);station.Reload();}
    for(bool successful:{false,true}){
        Station station;station.selected=105;
        const bool added=coaches::RegenerateOffers(station.offers,[&]{station.Clear();},[&]{
            station.storage[0]=999;station.offers.count=1;station.serialized.insert(999);
        },[&](auto){return successful;},[&]{station.Schedule();});
        Check(added==successful);
        Check(station.offers.count==(successful?1:10));
        Check(station.selected==(successful?0:105));
        Check(station.serialized.size()==(successful?1:10));station.Reload();
    }
    {Station station;station.selected=105;
        Check(!coaches::RegenerateOffers(station.offers,[&]{station.Clear();},[]{},[](auto){return false;},[&]{station.Schedule();}));
        Check(station.offers.count==10 && station.selected==105);station.Reload();}
    {Station station;station.Clear();
        Check(!coaches::RegenerateOffers(station.offers,[&]{station.Clear();},[]{},[](auto){return false;},[&]{station.Schedule();}));
        Check(station.offers.count==0);station.Reload();}
    // The reported failure: clear-all then restore retained addresses leaves
    // ten dangling references at save/load even if their memory still exists.
    {Station station;const auto retained=station.storage;station.Clear();station.storage=retained;station.offers.count=10;
        Check(station.serialized.empty());}
    std::puts("Coach replacement, selected offer cleanup, generation rollback and save references passed.");
}
