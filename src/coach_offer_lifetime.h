#pragma once
#include "owned_offers.h"

namespace coaches {
// Native clear calls each offer's destructive vtable callback BEFORE dropping
// its reference. A reference-count increment cannot keep that offer reusable.
// Temporarily expose only objects which really must be destroyed to native clear.
template<class Clear>
void DestroyDetachedOffers(GameOfferVector& installed,std::uintptr_t* removed,std::size_t count,Clear clear){
    const auto storage=installed;
    installed={storage.vtable,removed,count,count};
    clear(); // Native clear retains the vector's backing storage.
    installed=storage;
}

template<class Clear>
void ReplaceOffer(GameOfferVector& offers,std::size_t slot,std::uintptr_t replacement,Clear clear){
    auto removed=offers.data[slot];
    offers.data[slot]=replacement;
    DestroyDetachedOffers(offers,&removed,1,clear);
}

template<class Clear,class Fill,class Complete,class Schedule>
bool RegenerateOffers(GameOfferVector& offers,Clear clear,Fill fill,Complete complete,Schedule schedule){
    std::vector<std::uintptr_t> previous;
    if(offers.count)previous.assign(offers.data,offers.data+offers.count);
    // Transfer ownership out of the native list without destroying its objects.
    offers.count=0;
    clear();
    fill();
    bool added=false;
    for(std::uint64_t i=0;i<offers.count;++i)if(complete(offers.data[i]))added=true;
    if(added){
        DestroyDetachedOffers(offers,previous.data(),previous.size(),clear);
    }else{
        clear(); // Discard only the unsuccessful newly generated objects.
        std::copy(previous.begin(),previous.end(),offers.data);
        offers.count=previous.size();
    }
    // Clearing the detached list also removes station events. Rebuild events for
    // the final list, including the successful generation case.
    schedule();
    return added;
}
}
