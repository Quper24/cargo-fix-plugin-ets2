#pragma once
#include <cstdint>
#include <limits>
#include <string>
namespace delivery_time {
// Native callbacks return an absolute upper bound relative to the current game
// time. Scale the allowed duration, never the absolute game clock or offer expiry.
inline bool Quadruple(std::uint32_t now,std::uint32_t upper,std::uint32_t& result){
    if(upper<=now)return false;
    const auto value=std::uint64_t(now)+4*(std::uint64_t(upper)-now);
    if(value>std::numeric_limits<std::uint32_t>::max())return false;
    result=static_cast<std::uint32_t>(value);return true;
}
struct Fingerprint {
    std::string cargo,destination;
    std::uint32_t expiry=0,units=0,urgency=0;
    std::uint16_t distance=0;
    bool operator==(const Fingerprint& other)const{
        return cargo==other.cargo&&destination==other.destination&&expiry==other.expiry&&units==other.units&&urgency==other.urgency&&distance==other.distance;
    }
};
struct Record {std::string source;bool owned=false;std::uint32_t slot=0;Fingerprint offer;};
inline bool SameSlot(const Record& a,const Record& b){return a.source==b.source&&a.owned==b.owned&&a.slot==b.slot;}
}
