#include "custom_jobs.h"
#include "owned_candidate_probe.h"
#include "game_resolver.h"
#include <Windows.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <array>
#include <cstring>
#define Check(value) do { if (!(value)) { std::fprintf(stderr,"Catalog profile check failed: %d\n",__LINE__); std::exit(1); } } while(0)
#include "road_trip_factory_probe.h"
struct Fixture {
    const char* name; const char* sha256;
    std::uint32_t generator, fill, pools, guard_rva;
    std::array<unsigned char,7> guard;
};
constexpr Fixture fixtures[] = {
#include "custom_job_profiles.inc"
};
// Execute the validated candidate body from each real EXE with isolated inputs.
// Its sole external call (skill eligibility) is replaced by a true-return stub;
// trailer/body/load checks and borrowed-vector appends execute unchanged.
void CheckOwnedProbe(const std::vector<unsigned char>& image,const CreationCode& creation){
    using namespace custom_jobs;
    constexpr std::size_t length=0x191,call=0x93,stub=0x300;
    Check(creation.owned_candidates+length<image.size());
    auto* native=static_cast<unsigned char*>(VirtualAlloc(nullptr,4096,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));Check(native);
    std::memcpy(native,image.data()+creation.owned_candidates,length);
    Check(native[call]==0xe8 && native[length-1]==0xc3);
    const auto relative=static_cast<std::int32_t>(stub-call-5);
    std::memcpy(native+call+1,&relative,4);native[stub]=0xb0;native[stub+1]=1;native[stub+2]=0xc3;
    DWORD protection=0;Check(VirtualProtect(native,4096,PAGE_EXECUTE_READ,&protection));
    FlushInstructionCache(GetCurrentProcess(),native,4096);
    alignas(8) std::array<unsigned char,0xc0> cargo{},trailer{};
    auto put=[](auto& bytes,std::size_t offset,const auto& value){std::memcpy(bytes.data()+offset,&value,sizeof(value));};
    const std::uint64_t body=11,one=1;
    put(cargo,0x68,1.f);put(cargo,0x98,1.f);
    put(cargo,0xa8,reinterpret_cast<std::uintptr_t>(&body));put(cargo,0xb0,one);
    put(trailer,0x48,body);put(trailer,0x40,100.f);put(trailer,0x30,100.f);
    std::uintptr_t input=reinterpret_cast<std::uintptr_t>(cargo.data()),output=0;
    OwnedCargoProbe probe(&input,1);
    struct Vector {std::uintptr_t vtable=0;std::uintptr_t* data;std::uint64_t count=0,capacity=1;} values{0,&output,0,1};
    using Collect=std::uint64_t(__fastcall*)(std::uintptr_t,Vector*,std::uintptr_t,std::uintptr_t);
    const auto collect=reinterpret_cast<Collect>(native);
    const auto trailer_address=reinterpret_cast<std::uintptr_t>(trailer.data());
    Reader reader=[](std::uintptr_t at,void* out,std::size_t size){std::memcpy(out,reinterpret_cast<const void*>(at),size);return true;};
    Check(CargoFitsTrailer(reader,input,trailer_address));
    Check(collect(probe.Address(),&values,0,trailer_address)==1 && values.count==1 && output==input);
    values.count=0;output=0;put(trailer,0x48,std::uint64_t{22});
    Check(!CargoFitsTrailer(reader,input,trailer_address));
    Check(collect(probe.Address(),&values,0,trailer_address)==0 && !values.count && !output);
    put(trailer,0x48,body);put(trailer,0x40,0.f);
    Check(!CargoFitsTrailer(reader,input,trailer_address));
    Check(collect(probe.Address(),&values,0,trailer_address)==0 && !values.count);
    put(trailer,0x40,100.f);put(cargo,0x68,0.f);
    Check(CargoFitsTrailer(reader,input,trailer_address)); // Spawn probability is irrelevant to a direct job.
    Check(collect(probe.Address(),&values,0,trailer_address)==0 && !values.count);
    put(cargo,0x68,1.f);probe.definition.flags=0;
    Check(collect(probe.Address(),&values,0,trailer_address)==0 && !values.count);
    Check(VirtualFree(native,0,MEM_RELEASE));
}
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
    const Fixture* fixture=nullptr;
    for(const auto& item:fixtures) if(path.parent_path().filename()==item.name)fixture=&item;
    Check(fixture);
    std::uint32_t pools=0; std::string error;
    Check(ResolveCatalogPools(image.data(),image.size(),pools,error) && pools==fixture->pools);
    GameProfile core{};
    Check(ResolveGameProfile(image.data(),image.size(),path.filename()==L"amtrucks.exe"?GameKind::ATS:GameKind::ETS2,core,error));
    CreationCode creation;
    if(!ResolveCreationCode(image.data(),image.size(),core,creation,error))std::fprintf(stderr,"Creation resolver: %s\n",error.c_str());
    Check(creation.base_slot && creation.base_factory && creation.owned_candidates && creation.free_event_node && creation.physical_update);
    CheckOwnedProbe(image,creation);
    image[creation.destinations]^=1;
    CreationCode rejected;
    Check(!ResolveCreationCode(image.data(),image.size(),core,rejected,error) && !rejected.base_slot);
    image[creation.destinations]^=1;
    CatalogNameCode names;
    if (!ResolveCatalogNames(image.data(),image.size(),names,error)) std::fprintf(stderr,"Name resolver: %s\n",error.c_str());
    Check(names.localize && names.buffer_vtable && names.city_location && names.city_localized_flag);
    const auto expected_names=names;
    image[expected_names.localize]^=1;
    Check(!ResolveCatalogNames(image.data(),image.size(),names,error) && !names.localize);
    Check(ResolveCatalogPools(image.data(),image.size(),pools,error)); // Names may fail without disabling the catalog.
    image[expected_names.localize]^=1;
    std::uint64_t reserve_pointer=0;
    std::memcpy(&reserve_pointer,image.data()+expected_names.buffer_vtable+16,8);
    const auto wrong_reserve=reserve_pointer+1;
    std::memcpy(image.data()+expected_names.buffer_vtable+16,&wrong_reserve,8);
    Check(!ResolveCatalogNames(image.data(),image.size(),names,error) && !names.localize);
    std::memcpy(image.data()+expected_names.buffer_vtable+16,&reserve_pointer,8);
    bool changed_property=false;
    for(unsigned n=0;n<nt->FileHeader.NumberOfSections && !changed_property;++n) {
        const auto& s=sections[n];
        if(!(s.Characteristics&IMAGE_SCN_MEM_WRITE))continue;
        for(std::uint64_t at=s.VirtualAddress+32;at+8<=std::uint64_t(s.VirtualAddress)+s.Misc.VirtualSize;at+=8) {
            std::uint64_t pointer=0;std::memcpy(&pointer,image.data()+at,8);
            if(pointer<nt->OptionalHeader.ImageBase)continue;
            const auto rva=pointer-nt->OptionalHeader.ImageBase;
            constexpr char property[]="city_name_localized";
            if(rva>image.size()-sizeof(property) || std::memcmp(image.data()+rva,property,sizeof(property)))continue;
            image[at-24]^=1;
            Check(!ResolveCatalogNames(image.data(),image.size(),names,error));
            image[at-24]^=1;changed_property=true;break;
        }
    }
    Check(changed_property && ResolveCatalogNames(image.data(),image.size(),names,error));
    custom_jobs::Initialize(reinterpret_cast<std::uintptr_t>(image.data()),image.size(),nullptr);
    Check(custom_jobs::Snapshot()->supported);
    Check(custom_jobs::Snapshot()->names_supported);
    Check(custom_jobs::Snapshot()->availability==custom_jobs::Availability::Ready);
    custom_jobs::Observe(1,true); custom_jobs::Observe(1,false);
    Check(custom_jobs::Snapshot()->supported && !custom_jobs::Snapshot()->scanned);
    auto* mapped_nt=reinterpret_cast<IMAGE_NT_HEADERS64*>(image.data()+dos->e_lfanew);
    mapped_nt->FileHeader.TimeDateStamp ^= 0x12345678;
    // Change actual patch/build version resource fields. Neither metadata nor hash gates the catalog.
    bool changed=false;
    for(unsigned n=0;n<nt->FileHeader.NumberOfSections;++n) {
        const auto& s=sections[n];
        if(std::memcmp(s.Name,".rsrc",5))continue;
        for(std::uint64_t at=s.VirtualAddress;at+sizeof(VS_FIXEDFILEINFO)<=std::uint64_t(s.VirtualAddress)+s.Misc.VirtualSize;++at) {
            VS_FIXEDFILEINFO version{}; std::memcpy(&version,image.data()+at,sizeof(version));
            if(version.dwSignature!=0xfeef04bd)continue;
            version.dwFileVersionLS=0x7fff1234; version.dwProductVersionLS=0x7fff1234;
            std::memcpy(image.data()+at,&version,sizeof(version)); changed=true; break;
        }
    }
    Check(changed && ResolveCatalogPools(image.data(),image.size(),pools,error));
    RoadTripCode road{};
    if(path.filename()==L"amtrucks.exe"){
        if(!ResolveRoadTripCode(image.data(),image.size(),GameKind::ATS,road,error)){
            std::fprintf(stderr,"Road Trip: %s\n",error.c_str());std::exit(1);
        }
        const auto expected_road=road;
        CheckRoadTripFactory(image,road);
        Check(!ResolveRoadTripCode(image.data(),image.size(),GameKind::ETS2,road,error));
        for(auto address:{expected_road.generate,expected_road.factory+8,expected_road.select_car+8,expected_road.schedule+8,expected_road.remove_event+8,expected_road.lookup_car+8}){
            image[address]^=1;
            Check(!ResolveRoadTripCode(image.data(),image.size(),GameKind::ATS,road,error));
            image[address]^=1;
        }
        Check(ResolveRoadTripCode(image.data(),image.size(),GameKind::ATS,road,error));
    }else Check(!ResolveRoadTripCode(image.data(),image.size(),GameKind::ETS2,road,error));
    Check(ResolveCatalogNames(image.data(),image.size(),names,error) && names.localize==expected_names.localize);
    custom_jobs::Initialize(reinterpret_cast<std::uintptr_t>(image.data()),image.size(),nullptr);
    Check(custom_jobs::Snapshot()->supported);
    // A synthetic compatible update moves every function/global to a new RVA.
    constexpr DWORD shift=0x10000;
    std::vector<unsigned char> moved(image.size()+shift);
    std::copy_n(image.data(),nt->OptionalHeader.SizeOfHeaders,moved.data());
    auto* moved_nt=reinterpret_cast<IMAGE_NT_HEADERS64*>(moved.data()+dos->e_lfanew);
    auto* moved_sections=IMAGE_FIRST_SECTION(moved_nt);
    for(unsigned n=0;n<nt->FileHeader.NumberOfSections;++n) {
        const auto& s=sections[n];
        std::copy_n(image.data()+s.VirtualAddress,std::max(s.Misc.VirtualSize,s.SizeOfRawData),moved.data()+s.VirtualAddress+shift);
        moved_sections[n].VirtualAddress+=shift;
    }
    moved_nt->OptionalHeader.SizeOfImage+=shift;
    auto& directory=moved_nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    directory.VirtualAddress+=shift;
    auto* functions=reinterpret_cast<RUNTIME_FUNCTION*>(moved.data()+directory.VirtualAddress);
    for(unsigned n=0;n<directory.Size/sizeof(*functions);++n) {
        functions[n].BeginAddress+=shift; functions[n].EndAddress+=shift; functions[n].UnwindData+=shift;
    }
    Check(ResolveCatalogPools(moved.data(),moved.size(),pools,error) && pools==fixture->pools+shift);
    GameProfile moved_core{};CreationCode moved_creation;
    Check(ResolveGameProfile(moved.data(),moved.size(),path.filename()==L"amtrucks.exe"?GameKind::ATS:GameKind::ETS2,moved_core,error));
    Check(ResolveCreationCode(moved.data(),moved.size(),moved_core,moved_creation,error));
    Check(moved_creation.base_slot==creation.base_slot+shift && moved_creation.allocator_array==creation.allocator_array+shift);
    if(path.filename()==L"amtrucks.exe"){
        // Metadata/vtables contain absolute image pointers; move those with their sections.
        const auto preferred=moved_nt->OptionalHeader.ImageBase;
        for(unsigned n=0;n<moved_nt->FileHeader.NumberOfSections;++n){
            const auto& s=moved_sections[n];if(s.Characteristics&IMAGE_SCN_MEM_EXECUTE)continue;
            for(std::uint64_t at=s.VirtualAddress;at+8<=std::uint64_t(s.VirtualAddress)+std::max(s.Misc.VirtualSize,s.SizeOfRawData);at+=8){
                std::uint64_t pointer=0;std::memcpy(&pointer,moved.data()+at,8);
                if(pointer>=preferred+nt->OptionalHeader.SizeOfHeaders&&pointer<preferred+image.size()){
                    pointer+=shift;std::memcpy(moved.data()+at,&pointer,8);
                }
            }
        }
        RoadTripCode moved_road{};Check(ResolveRoadTripCode(moved.data(),moved.size(),GameKind::ATS,moved_road,error));
        Check(moved_road.generate==road.generate+shift&&moved_road.registry==road.registry+shift&&moved_road.vehicle_registry==road.vehicle_registry+shift);
    }
    custom_jobs::Initialize(reinterpret_cast<std::uintptr_t>(moved.data()),moved.size(),nullptr);
    Check(custom_jobs::Snapshot()->supported);
    image[fixture->guard_rva]^=1;
    custom_jobs::Initialize(reinterpret_cast<std::uintptr_t>(image.data()),image.size(),nullptr);
    Check(!custom_jobs::Snapshot()->supported && !custom_jobs::Snapshot()->scanned);
    image[fixture->guard_rva]^=1;
    // A changed field offset and a changed internal branch must fail too, not just the entry bytes.
    for(const auto address : {fixture->guard_rva+0x15, fixture->guard_rva-0x74}) {
        image[address]^=1;
        Check(!ResolveCatalogPools(image.data(),image.size(),pools,error) && pools==0);
        image[address]^=1;
    }
    std::int32_t displacement=0;
    std::memcpy(&displacement,image.data()+fixture->guard_rva+3,sizeof(displacement)); ++displacement;
    std::memcpy(image.data()+fixture->guard_rva+3,&displacement,sizeof(displacement));
    Check(!ResolveCatalogPools(image.data(),image.size(),pools,error));
    Check(!ResolveCatalogPools(image.data(),image.size()-1,pools,error));
    custom_jobs::Shutdown();
    std::puts("Catalog code: patch/build metadata and relocated RVAs accepted; modified instructions, invalid pool references and truncation rejected.");
}
