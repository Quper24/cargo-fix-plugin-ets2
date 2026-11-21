#pragma once
// Run the validated native constructor in isolation. Only its allocator is
// stubbed; the real constructor initializes every string/link/offer field.
inline void CheckRoadTripFactory(const std::vector<unsigned char>& image,const RoadTripCode& road){
    constexpr std::size_t size=0xf6,stub=0x200,table=0x300,object=0x1000;
    auto* memory=static_cast<unsigned char*>(VirtualAlloc(nullptr,8192,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));Check(memory);
    Check(road.factory+size<=image.size());std::memcpy(memory,image.data()+road.factory,size);
    Check(memory[0x10]==0xe8&&memory[size-1]==0xc3);
    auto relative=static_cast<std::int32_t>(stub-0x10-5);std::memcpy(memory+0x11,&relative,4);
    memory[stub]=0x48;memory[stub+1]=0xb8;const auto address=reinterpret_cast<std::uintptr_t>(memory+object);
    std::memcpy(memory+stub+2,&address,8);memory[stub+10]=0xc3;
    for(const auto offset:{0x9u,0x23u,0x2du,0x47u,0x67u,0x75u}){
        Check(memory[offset]==0x48&&memory[offset+1]==0x8d&&(memory[offset+2]&0xc7)==5);
        relative=static_cast<std::int32_t>(table-offset-7);std::memcpy(memory+offset+3,&relative,4);
    }
    DWORD old=0;Check(VirtualProtect(memory,4096,PAGE_EXECUTE_READ,&old));FlushInstructionCache(GetCurrentProcess(),memory,4096);
    std::array<unsigned char,32> pool{};pool[0x10]=1;
    const auto result=reinterpret_cast<std::uintptr_t(__fastcall*)(void*)>(memory)(pool.data());Check(result==address);
    auto word=[&](unsigned offset){std::uint64_t value=0;std::memcpy(&value,memory+object+offset,8);return value;};
    Check(word(0x18)==address+0x28&&word(0x50)==address+0x60);
    Check(word(0x20)==0x2000000000ull&&word(0x58)==0x2000000000ull);
    Check(word(0x80)==UINT64_MAX&&word(0x98)==0&&word(0xa0)==0&&word(0xc8)==0);
    Check(memory[object+0x28]==0&&memory[object+0x60]==0);
    Check(VirtualFree(memory,0,MEM_RELEASE));
}
