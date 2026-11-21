#include "game_resolver.h"
#include <Windows.h>
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <vector>
#include <array>
#include <cstdlib>
#include <cstring>
#include "coach_builder_probe.h"
#include "coach_route.h"
int main(int argc,char** argv){
    std::vector<std::string> route;
    for(unsigned i=0;i<24;++i)route.push_back("city."+std::to_string(i));
    if(!coaches::ValidRoute(route))return 7;
    route.push_back("city.24");if(coaches::ValidRoute(route))return 8;
    route.resize(2);route[1]=route[0];if(coaches::ValidRoute(route))return 9;
    route.resize(1);if(coaches::ValidRoute(route))return 10;
    if(argc!=2)return 2;
    std::ifstream in(argv[1],std::ios::binary);
    std::vector<unsigned char> raw((std::istreambuf_iterator<char>(in)),{});
    if(raw.size()<0x1000)return 3;
    const auto* dos=reinterpret_cast<const IMAGE_DOS_HEADER*>(raw.data());
    const auto* nt=reinterpret_cast<const IMAGE_NT_HEADERS64*>(raw.data()+dos->e_lfanew);
    std::vector<unsigned char> image(nt->OptionalHeader.SizeOfImage);
    std::copy_n(raw.data(),nt->OptionalHeader.SizeOfHeaders,image.data());
    const auto* sections=IMAGE_FIRST_SECTION(nt);
    for(unsigned n=0;n<nt->FileHeader.NumberOfSections;++n){const auto& s=sections[n];
        std::copy_n(raw.data()+s.PointerToRawData,s.SizeOfRawData,image.data()+s.VirtualAddress);
    }
    CoachesCode code{};std::string reason;
    if(!ResolveCoachesCode(image.data(),image.size(),GameKind::ETS2,code,reason)){
        std::fprintf(stderr,"Coaches resolution failed: %s\n",reason.c_str());return 1;
    }
    std::printf("Coaches checked: fill=%x, init=%x, append=%x, commit=%x\n",code.fill,code.planner_init,code.append,code.commit);
    coach_probe::Run(image,code);
    const auto saved=code;
    image[saved.append]^=1;
    if(ResolveCoachesCode(image.data(),image.size(),GameKind::ETS2,code,reason))return 4;
    if(code.fill||code.append)return 5;
    image[saved.append]^=1;
    if(ResolveCoachesCode(image.data(),image.size(),GameKind::ATS,code,reason))return 6;
    return 0;
}
