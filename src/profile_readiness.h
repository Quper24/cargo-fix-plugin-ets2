#pragma once
#include "company_list.h"
#include <cstring>
struct EconomySnapshot {
    std::uintptr_t economy=0, player=0, companies=0;
    std::uint64_t count=0;
    bool operator==(const EconomySnapshot& other) const {
        return economy==other.economy && player==other.player && companies==other.companies && count==other.count;
    }
};
inline bool ReadEconomySnapshot(std::uintptr_t economy, EconomySnapshot& out) {
    out={};
    if(!ReadableMemory(economy,0x1a0))return false;
    EconomySnapshot value{};value.economy=economy;
    std::memcpy(&value.player,reinterpret_cast<const void*>(economy+0x18),sizeof(value.player));
    std::memcpy(&value.companies,reinterpret_cast<const void*>(economy+0x28),sizeof(value.companies));
    std::memcpy(&value.count,reinterpret_cast<const void*>(economy+0x30),sizeof(value.count));
    // A garage profile may have no active truck/player reference. Freight generation only needs the loaded economy.
    if((value.player && !ReadableMemory(value.player,0x80)) || !ValidCompanyList(value.companies,value.count))return false;
    out=value;return true;
}
class ProfileReadiness {
public:
    bool Observe(const EconomySnapshot& snapshot,bool valid,bool restart) {
        if(!valid || restart) { previous_={};stable_=0;return false; }
        stable_=(stable_ && snapshot==previous_)?2:1;previous_=snapshot;return stable_>=2;
    }
private:
    EconomySnapshot previous_{};unsigned stable_=0;
};
