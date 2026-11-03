#include "profile_readiness.h"
#include <array>
#include <cstdlib>
#include <cstring>
void Check(bool ok) { if(!ok) std::abort(); }
int main() {
    alignas(8) std::array<unsigned char,0x1a0> economy{};
    alignas(8) std::array<unsigned char,0x80> player{}; // No truck and no selected trailer.
    std::array<std::uintptr_t,2> companies{1,2};
    auto base=reinterpret_cast<std::uintptr_t>(economy.data());
    auto player_address=reinterpret_cast<std::uintptr_t>(player.data());
    auto list=reinterpret_cast<std::uintptr_t>(companies.data());std::uint64_t count=companies.size();
    std::memcpy(economy.data()+0x18,&player_address,sizeof(player_address));
    std::memcpy(economy.data()+0x28,&list,sizeof(list));std::memcpy(economy.data()+0x30,&count,sizeof(count));
    EconomySnapshot snapshot{};ProfileReadiness ready;
    Check(ReadEconomySnapshot(base,snapshot));
    Check(!ready.Observe(snapshot,true,false));Check(ready.Observe(snapshot,true,false)); // Loaded garage without a truck is allowed.
    Check(!ready.Observe(snapshot,true,true)); // Save/load restart is rejected.
    Check(!ready.Observe(snapshot,true,false));Check(ready.Observe(snapshot,true,false));
    auto changed=snapshot;changed.economy+=8;
    Check(!ready.Observe(changed,true,false));Check(ready.Observe(changed,true,false));
    Check(!ReadEconomySnapshot(0,snapshot));Check(!ready.Observe(snapshot,false,false));
    count=0;std::memcpy(economy.data()+0x30,&count,sizeof(count));Check(!ReadEconomySnapshot(base,snapshot));
    count=companies.size();std::memcpy(economy.data()+0x30,&count,sizeof(count));
    player_address=0;std::memcpy(economy.data()+0x18,&player_address,sizeof(player_address));Check(ReadEconomySnapshot(base,snapshot));
    Check(!ready.Observe(snapshot,true,false));Check(ready.Observe(snapshot,true,false)); // No active player/truck reference is required for Quick jobs.
    player_address=1;std::memcpy(economy.data()+0x18,&player_address,sizeof(player_address));Check(!ReadEconomySnapshot(base,snapshot));
    Check(!ReadEconomySnapshot(UINTPTR_MAX-8,snapshot));
}
