#pragma once
#include "delivery_time_policy.h"
// Real game window calculations, copied to isolated executable memory. Normal
// paths have no external calls; only their two read-only float constants move.
inline void CheckDeliveryWindows(const std::vector<unsigned char>& image,const DeliveryTimeCode& code){
    using NativeWindow=void(__fastcall*)(void*,std::uint32_t*,std::uint32_t*,void*);
    for(bool owned:{false,true}){
        const auto rva=owned?code.owned_window:code.freight_window;
        const std::size_t length=owned?0x148:0x101;
        auto* executable=static_cast<unsigned char*>(VirtualAlloc(nullptr,4096,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));Check(executable);
        Check(rva+length<=image.size());std::memcpy(executable,image.data()+rva,length);
        const std::vector<unsigned> constants=owned?std::vector<unsigned>{0x6f,0xf0}:std::vector<unsigned>{0x58};
        for(unsigned n=0;n<constants.size();++n){const auto at=constants[n];
            Check(!std::memcmp(executable+at,"\xf3\x0f\x59\x05",4));
            std::int32_t old=0;std::memcpy(&old,executable+at+4,4);
            const auto original=std::int64_t(rva)+at+8+old;Check(original>=0&&original+4<=image.size());
            std::memcpy(executable+0x400+n*4,image.data()+original,4);
            const auto relative=static_cast<std::int32_t>(0x400+n*4-at-8);std::memcpy(executable+at+4,&relative,4);
        }
        DWORD protection=0;Check(VirtualProtect(executable,4096,PAGE_EXECUTE_READ,&protection));FlushInstructionCache(GetCurrentProcess(),executable,4096);
        std::array<unsigned char,0x1a0> economy{};std::array<unsigned char,0xe0> settings{},offer{},cargo{};
        std::array<unsigned char,0x58> player{};std::array<std::uint32_t,3> windows{120,60,30};
        auto put=[](auto& bytes,unsigned at,auto value){Check(at+sizeof(value)<=bytes.size());std::memcpy(bytes.data()+at,&value,sizeof(value));};
        put(economy,0x170,reinterpret_cast<std::uintptr_t>(settings.data()));put(economy,0x18,reinterpret_cast<std::uintptr_t>(player.data()));put(economy,0x19c,10000u);
        put(settings,0x88,60.f);put(settings,0xc0,660u);put(settings,0xc4,540u);
        put(settings,0xd0,reinterpret_cast<std::uintptr_t>(windows.data()));put(settings,0xd8,std::uint64_t(3));
        put(offer,0x4c,1u);put(offer,0x50,std::uint16_t(300));put(offer,0x52,std::uint16_t(20));
        put(offer,0x58,reinterpret_cast<std::uintptr_t>(cargo.data()));put(offer,0xcc,2u);put(cargo,0xc8,30.f);
        for(unsigned driving:{0u,600u}){put(player,0x50,driving);
            std::uint32_t lower=0,upper=0;reinterpret_cast<NativeWindow>(executable)(offer.data(),&lower,&upper,economy.data());
            // 300 minutes driving + 20 ferry; one rest with 600 minutes already driven.
            const auto expected=10000u+320u+(driving?540u:0u)+(owned?1u:0u);
            Check(lower==expected-15&&upper==expected+45);
            const auto native_lower=lower,native_upper=upper;std::uint32_t extended=0;
            Check(delivery_time::Quadruple(10000,upper,extended));upper=extended;
            Check(lower==native_lower&&upper-10000==4*(native_upper-10000));
            // Repeated UI queries recompute from native values; x4 never accumulates to x16.
            reinterpret_cast<NativeWindow>(executable)(offer.data(),&lower,&upper,economy.data());
            Check(upper==native_upper&&delivery_time::Quadruple(10000,upper,extended)&&extended-10000==4*(native_upper-10000));
        }
        Check(VirtualFree(executable,0,MEM_RELEASE));
    }
}
