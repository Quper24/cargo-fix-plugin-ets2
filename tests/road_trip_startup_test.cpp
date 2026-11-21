#include "road_trip.h"
#include <Windows.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string_view>
#define Check(value) do{if(!(value)){std::fprintf(stderr,"Road Trip startup check failed: %d\n",__LINE__);std::exit(1);}}while(0)
// Rendering/notification endpoints are not used by initialization or detection.
namespace overlay_ui{void Request(int){}void SetProgress(std::uint64_t,std::uint64_t){}}
namespace notifications{bool Russian(){return true;}void Show(const std::wstring&){}}
namespace custom_jobs{bool ContainsName(std::string_view value,std::string_view query){return value.find(query)!=std::string_view::npos;}}
int main(int argc,char** argv){
    Check(argc==2);std::ifstream input(argv[1],std::ios::binary);Check(input.good());
    std::vector<unsigned char> raw((std::istreambuf_iterator<char>(input)),{});
    const auto* dos=reinterpret_cast<const IMAGE_DOS_HEADER*>(raw.data());
    const auto* nt=reinterpret_cast<const IMAGE_NT_HEADERS64*>(raw.data()+dos->e_lfanew);
    std::vector<unsigned char> image(nt->OptionalHeader.SizeOfImage);
    std::copy_n(raw.data(),nt->OptionalHeader.SizeOfHeaders,image.data());
    const auto* sections=IMAGE_FIRST_SECTION(nt);
    for(unsigned n=0;n<nt->FileHeader.NumberOfSections;++n){const auto& s=sections[n];
        Check(std::uint64_t(s.PointerToRawData)+s.SizeOfRawData<=raw.size());Check(std::uint64_t(s.VirtualAddress)+s.SizeOfRawData<=image.size());
        std::copy_n(raw.data()+s.PointerToRawData,s.SizeOfRawData,image.data()+s.VirtualAddress);
    }
    const auto base=reinterpret_cast<std::uintptr_t>(image.data());GameProfile core{};CreationCode creation{};RoadTripCode code{};std::string error;
    Check(ResolveGameProfile(image.data(),image.size(),GameKind::ATS,core,error));
    Check(ResolveCreationCode(image.data(),image.size(),core,creation,error));
    Check(ResolveRoadTripCode(image.data(),image.size(),GameKind::ATS,code,error));
    Check(code.schedule==creation.schedule); // Both modules share this exact scheduler.
    road_trip::Initialize(base,image.size(),GameKind::ATS,core,creation,nullptr);
    Check(road_trip::Snapshot()->available);
    // A later truck hook changes the scheduler entry. Road Trip keeps the code
    // resolved before installation instead of rejecting our own detour.
    image[code.schedule]=0xe9;
    std::fill_n(image.data()+code.schedule+1,4,0);
    RoadTripCode after_hook{};Check(!ResolveRoadTripCode(image.data(),image.size(),GameKind::ATS,after_hook,error));
    Check(road_trip::Snapshot()->available);
    road_trip::Observe(0,false);Check(road_trip::Snapshot()->status==road_trip::SessionStatus::NotLoaded);
    std::vector<unsigned char> economy(0x1e00),generator(0x70),registry(0x3100),storage(0x900);
    auto put=[](auto& data,std::size_t offset,auto value){Check(offset+sizeof(value)<=data.size());std::memcpy(data.data()+offset,&value,sizeof(value));};
    put(generator,8,0x80000000u);put(economy,0x1d48,reinterpret_cast<std::uintptr_t>(generator.data()));
    const auto econ=reinterpret_cast<std::uintptr_t>(economy.data());
    // No loaded definitions => distinguish content/DLC availability from code.
    road_trip::Observe(econ,true);Check(road_trip::Snapshot()->available&&road_trip::Snapshot()->status==road_trip::SessionStatus::ContentUnavailable);
    std::uintptr_t definition=0; // Detection reads the list, not native constructors.
    put(image,code.registry,reinterpret_cast<std::uintptr_t>(registry.data()));
    put(registry,0x2ff8,reinterpret_cast<std::uintptr_t>(storage.data()));
    put(storage,0x8d0,reinterpret_cast<std::uintptr_t>(&definition));put(storage,0x8d8,std::uint64_t(1));
    road_trip::Observe(econ,true);Check(road_trip::Snapshot()->status==road_trip::SessionStatus::Ready);
    road_trip::Observe(econ,false);Check(road_trip::Snapshot()->status==road_trip::SessionStatus::NotLoaded);
    road_trip::Reset();
    std::puts("Road Trip survives our later scheduler detour; unloaded profile, missing content and loaded content are distinct.");
}
