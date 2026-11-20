#include "custom_jobs.h"
#include "game_resolver.h"
#include <Windows.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#define Check(value) do { if (!(value)) { std::fprintf(stderr,"Catalog profile check failed: %d\n",__LINE__); std::exit(1); } } while(0)
int wmain(int argc, wchar_t** argv) {
    Check(argc==2);
    const std::filesystem::path path(argv[1]);
    std::ifstream file(path,std::ios::binary); Check(file.good());
    std::vector<unsigned char> raw((std::istreambuf_iterator<char>(file)),{});
    Check(raw.size()>sizeof(IMAGE_DOS_HEADER));
    const auto* dos=reinterpret_cast<const IMAGE_DOS_HEADER*>(raw.data());
    Check(dos->e_lfanew>0 && std::size_t(dos->e_lfanew)+sizeof(IMAGE_NT_HEADERS64)<raw.size());
    const auto* nt=reinterpret_cast<const IMAGE_NT_HEADERS64*>(raw.data()+dos->e_lfanew);
    std::vector<unsigned char> image(nt->OptionalHeader.SizeOfImage);
    std::copy_n(raw.data(),nt->OptionalHeader.SizeOfHeaders,image.data());
    const auto* sections=IMAGE_FIRST_SECTION(nt);
    for(unsigned n=0;n<nt->FileHeader.NumberOfSections;++n) {
        const auto& s=sections[n];
        Check(std::uint64_t(s.PointerToRawData)+s.SizeOfRawData<=raw.size());
        Check(std::uint64_t(s.VirtualAddress)+s.SizeOfRawData<=image.size());
        std::copy_n(raw.data()+s.PointerToRawData,s.SizeOfRawData,image.data()+s.VirtualAddress);
    }
    GameProfile profile{}; std::string error;
    const auto kind=path.filename()==L"amtrucks.exe"?GameKind::ATS:GameKind::ETS2;
    Check(ResolveGameProfile(image.data(),image.size(),kind,profile,error));
    const auto base=reinterpret_cast<std::uintptr_t>(image.data());
    custom_jobs::Initialize(base,profile,path,nullptr);
    Check(custom_jobs::Snapshot()->supported);
    Check(custom_jobs::Snapshot()->availability==custom_jobs::Availability::Ready);
    custom_jobs::Observe(1,true); custom_jobs::Observe(1,false);
    Check(custom_jobs::Snapshot()->supported && !custom_jobs::Snapshot()->scanned);
    auto incompatible=profile; ++incompatible.generator;
    custom_jobs::Initialize(base,incompatible,path,nullptr);
    Check(!custom_jobs::Snapshot()->supported && custom_jobs::Snapshot()->availability==custom_jobs::Availability::CoreMismatch);
    std::fill(image.begin(),image.end(),0); // The on-disk hash still matches; the live code must be checked too.
    custom_jobs::Initialize(base,profile,path,nullptr);
    Check(!custom_jobs::Snapshot()->supported && custom_jobs::Snapshot()->availability==custom_jobs::Availability::CodeMismatch);
    custom_jobs::Shutdown();
    std::puts("Catalog profile: exact EXE accepted, readiness reset preserved support, wrong core and modified live code rejected.");
}
