#include "game_resolver.h"
#include <Windows.h>
#include <algorithm>
#include <cstring>
#include <iterator>
#include <limits>
#include <vector>

namespace {
// kind: 0 literal, 1 RIP reference, 2 external relative call/jump, 3 graph edge.
struct Instruction {
    unsigned char bytes[15];
    unsigned char size, offset, width, kind;
    short next, target, callee, symbol;
};
struct Signature { const char* name; const Instruction* instructions; std::size_t count; };
namespace code161 {
#include "game_signatures_161.inc"
}
namespace code162 {
#include "game_signatures_162.inc"
}
namespace catalog162 {
#include "custom_job_signatures_162.inc"
}
namespace catalog_names162 {
#include "custom_name_signatures_162.inc"
}
namespace creation162 {
#include "custom_creation_signatures_162.inc"
}
namespace road_trip162 {
#include "road_trip_signatures_162.inc"
}
namespace delivery_time162 {
#include "delivery_time_signatures_162.inc"
}
struct SignatureSet {
    const char* name;
    const char* ets2_name;
    const char* ats_name;
    const Signature* signatures;
    std::size_t count;
    unsigned symbol_count, economy_source_load, generator_clear_call;
    unsigned generator_budget_branch, generator_budget_immediate, generator_budget_stack;
};
static constexpr SignatureSet layouts[] = {
    {"1.61 layout", "ETS2 compatible 1.61 code", "ATS compatible 1.61 code",
     code161::signatures, std::size(code161::signatures), code161::symbol_count,
     code161::economy_source_load, code161::generator_clear_call, code161::generator_budget_branch,
     code161::generator_budget_immediate, code161::generator_budget_stack},
    {"1.62 layout", "ETS2 compatible 1.62 code", "ATS compatible 1.62 code",
     code162::signatures, std::size(code162::signatures), code162::symbol_count,
     code162::economy_source_load, code162::generator_clear_call, code162::generator_budget_branch,
     code162::generator_budget_immediate, code162::generator_budget_stack},
};

bool Range(std::size_t start, std::size_t count, std::size_t size) {
    return start <= size && count <= size - start;
}
template<class T> T Read(const unsigned char* p) {
    T value; std::memcpy(&value, p, sizeof(value)); return value;
}
struct Section { std::uint32_t rva, size, flags; };
struct Image {
    const unsigned char* data;
    std::size_t size;
    std::vector<Section> sections;
    std::vector<std::uint32_t> starts;
    bool In(std::uint32_t rva, std::size_t length, DWORD required, DWORD forbidden = 0) const {
        if (!Range(rva, length, size)) return false;
        for (const auto& s : sections)
            if (rva >= s.rva && Range(rva - s.rva, length, s.size) &&
                (s.flags & required) == required && !(s.flags & forbidden)) return true;
        return false;
    }
    bool Code(std::uint32_t rva, std::size_t length = 1) const {
        return In(rva, length, IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_EXECUTE, IMAGE_SCN_MEM_WRITE);
    }
    bool Parse() {
        if (!data || size < sizeof(IMAGE_DOS_HEADER) || Read<WORD>(data) != IMAGE_DOS_SIGNATURE) return false;
        const auto offset = Read<LONG>(data + offsetof(IMAGE_DOS_HEADER, e_lfanew));
        if (offset < 0 || !Range(offset, sizeof(IMAGE_NT_HEADERS64), size)) return false;
        const auto nt = Read<IMAGE_NT_HEADERS64>(data + offset);
        if (nt.Signature != IMAGE_NT_SIGNATURE || nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
            nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
            nt.FileHeader.SizeOfOptionalHeader != sizeof(IMAGE_OPTIONAL_HEADER64) ||
            nt.OptionalHeader.SizeOfImage != size || !nt.FileHeader.NumberOfSections ||
            nt.FileHeader.NumberOfSections > 96 ||
            nt.OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_EXCEPTION) return false;
        const auto section_offset = static_cast<std::size_t>(offset) + sizeof(IMAGE_NT_HEADERS64);
        if (!Range(section_offset, nt.FileHeader.NumberOfSections * sizeof(IMAGE_SECTION_HEADER), size)) return false;
        for (unsigned n = 0; n < nt.FileHeader.NumberOfSections; ++n) {
            const auto s = Read<IMAGE_SECTION_HEADER>(data + section_offset + n * sizeof(IMAGE_SECTION_HEADER));
            const auto length = std::max(s.Misc.VirtualSize, s.SizeOfRawData);
            if (!length || !Range(s.VirtualAddress, length, size)) return false;
            for (const auto& previous : sections)
                if (s.VirtualAddress < std::uint64_t(previous.rva) + previous.size &&
                    previous.rva < std::uint64_t(s.VirtualAddress) + length) return false;
            sections.push_back({s.VirtualAddress, length, s.Characteristics});
        }
        const auto directory = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
        if (!directory.Size || directory.Size % sizeof(RUNTIME_FUNCTION) ||
            !In(directory.VirtualAddress, directory.Size, IMAGE_SCN_MEM_READ, IMAGE_SCN_MEM_WRITE)) return false;
        std::uint32_t previous = 0;
        for (std::size_t n = 0; n < directory.Size; n += sizeof(RUNTIME_FUNCTION)) {
            const auto f = Read<RUNTIME_FUNCTION>(data + directory.VirtualAddress + n);
            if (f.BeginAddress <= previous || f.EndAddress <= f.BeginAddress ||
                !Code(f.BeginAddress, f.EndAddress - f.BeginAddress) ||
                !In(f.UnwindData, 1, IMAGE_SCN_MEM_READ)) return false;
            previous = f.BeginAddress;
            starts.push_back(f.BeginAddress);
        }
        return true;
    }
};

struct Reference { short callee, symbol; std::uint32_t address; };
bool CheckInstruction(const Image& image, const Instruction& ins, std::uint32_t pc,
                      std::uint32_t& destination, std::vector<Reference>& references) {
    if (!image.Code(pc, ins.size)) return false;
    for (unsigned b = 0; b < ins.size; ++b)
        if (!(ins.width && b >= ins.offset && b < ins.offset + ins.width) &&
            image.data[pc + b] != ins.bytes[b]) return false;
    destination = 0;
    if (!ins.width) return true;
    const auto displacement = ins.width == 1 ? Read<std::int8_t>(image.data + pc + ins.offset) :
                                              Read<std::int32_t>(image.data + pc + ins.offset);
    const auto address = static_cast<std::int64_t>(pc) + ins.size + displacement;
    if (address < 0 || address > UINT32_MAX) return false;
    destination = static_cast<std::uint32_t>(address);
    if (ins.kind == 1 ? !image.In(destination, 1, IMAGE_SCN_MEM_READ) : !image.Code(destination)) return false;
    if (ins.symbol >= 0 || ins.callee >= 0) references.push_back({ins.callee, ins.symbol, destination});
    return true;
}

bool Match(const Image& image, const Signature& signature, std::uint32_t start,
           bool block, std::vector<Reference>& references) {
    references.clear();
    std::vector<std::uint32_t> addresses(signature.count, 0);
    if (block) {
        auto pc = start;
        for (std::size_t n = 0; n < signature.count; ++n) {
            addresses[n] = pc; pc += signature.instructions[n].size;
        }
        for (std::size_t n = 0; n < signature.count; ++n) {
            std::uint32_t target = 0;
            const auto& ins = signature.instructions[n];
            if (!CheckInstruction(image, ins, addresses[n], target, references) ||
                (ins.target >= 0 && target != addresses[ins.target])) return false;
        }
        return true;
    }
    std::vector<std::size_t> pending{0};
    addresses[0] = start;
    auto bind = [&](short node, std::uint32_t pc) {
        if (node < 0) return true;
        if (addresses[node]) return addresses[node] == pc;
        // Different instructions must not alias, even after branch relocation.
        if (std::find(addresses.begin(), addresses.end(), pc) != addresses.end()) return false;
        addresses[node] = pc; pending.push_back(node); return true;
    };
    std::size_t visited = 0;
    while (!pending.empty()) {
        const auto n = pending.back(); pending.pop_back(); ++visited;
        const auto& ins = signature.instructions[n];
        const auto pc = addresses[n];
        std::uint32_t target = 0;
        if (!CheckInstruction(image, ins, pc, target, references) ||
            !bind(ins.next, pc + ins.size) || !bind(ins.target, target)) return false;
    }
    if (visited != signature.count) return false;
    std::vector<std::pair<std::uint32_t, unsigned>> ranges;
    for (std::size_t n = 0; n < signature.count; ++n) ranges.emplace_back(addresses[n], signature.instructions[n].size);
    std::sort(ranges.begin(), ranges.end());
    for (std::size_t n = 1; n < ranges.size(); ++n)
        if (std::uint64_t(ranges[n-1].first) + ranges[n-1].second > ranges[n].first) return false;
    return true;
}

void Guard(GameProfile& profile, unsigned index, const Image& image, std::uint32_t rva, unsigned length) {
    auto& guard = profile.guards[index]; guard.rva = rva; guard.length = length;
    std::copy_n(image.data + rva, length, guard.bytes.begin());
}

bool ResolveSet(const Image& image, const Signature* signatures, std::size_t count, unsigned symbol_count,
                std::vector<std::uint32_t>& resolved, std::vector<std::uint32_t>& symbols, std::string& reason) {
    resolved.assign(count,0); symbols.assign(symbol_count,0);
    std::vector<Reference> references, current;
    for (std::size_t n=0;n<count;++n) {
        unsigned matches=0;
        const auto& signature=signatures[n]; const auto& first=signature.instructions[0];
        for (const auto candidate:image.starts) {
            if (!image.Code(candidate,first.size) || std::memcmp(image.data+candidate,first.bytes,first.size)) continue;
            if (Match(image,signature,candidate,false,current)) {
                resolved[n]=candidate; references.insert(references.end(),current.begin(),current.end());
                if (++matches>1) break;
            }
        }
        if (matches!=1) { reason=std::string(matches?"ambiguous name signature: ":"incompatible name signature: ")+signature.name; return false; }
    }
    for (const auto& ref:references) {
        if ((ref.callee>=0 && ref.address!=resolved[ref.callee]) ||
            (ref.symbol>=0 && symbols[ref.symbol] && symbols[ref.symbol]!=ref.address)) {
            reason="inconsistent name reference graph"; return false;
        }
        if (ref.symbol>=0) symbols[ref.symbol]=ref.address;
    }
    return true;
}

std::uint32_t PointerRva(const Image& image, std::uint64_t address) {
    const auto actual=reinterpret_cast<std::uintptr_t>(image.data);
    if (address>=actual && address-actual<image.size) return static_cast<std::uint32_t>(address-actual);
    const auto pe=Read<std::uint32_t>(image.data+0x3c);
    const auto preferred=Read<IMAGE_NT_HEADERS64>(image.data+pe).OptionalHeader.ImageBase;
    if (address>=preferred && address-preferred<image.size) return static_cast<std::uint32_t>(address-preferred);
    return UINT32_MAX;
}
struct PropertyCheck { const char* name; std::uint64_t offset, type; };
bool PropertyGroup(const Image& image, const PropertyCheck* checks, unsigned count) {
    unsigned matches=0;
    for (const auto& section:image.sections) {
        if (!(section.flags&IMAGE_SCN_MEM_READ) || (section.flags&IMAGE_SCN_MEM_EXECUTE)) continue;
        for (std::uint64_t at=section.rva;at+std::uint64_t(count)*40<=std::uint64_t(section.rva)+section.size;at+=8) {
            if (Read<std::uint64_t>(image.data+at+8)!=checks[0].offset || Read<std::uint64_t>(image.data+at+16)!=checks[0].type) continue;
            const auto owner=Read<std::uint64_t>(image.data+at);
            if (!image.In(PointerRva(image,owner),8,IMAGE_SCN_MEM_READ,IMAGE_SCN_MEM_WRITE|IMAGE_SCN_MEM_EXECUTE)) continue;
            bool valid=true;
            for (unsigned n=0;n<count && valid;++n) {
                const auto p=at+n*40;
                const auto name=PointerRva(image,Read<std::uint64_t>(image.data+p+32));
                const auto length=std::strlen(checks[n].name)+1;
                valid=Read<std::uint64_t>(image.data+p)==owner && Read<std::uint64_t>(image.data+p+8)==checks[n].offset &&
                    Read<std::uint64_t>(image.data+p+16)==checks[n].type && image.In(name,length,IMAGE_SCN_MEM_READ,IMAGE_SCN_MEM_WRITE|IMAGE_SCN_MEM_EXECUTE) &&
                    !std::memcmp(image.data+name,checks[n].name,length);
            }
            if (valid && ++matches>1) return false;
        }
    }
    return matches==1;
}
}

GameKind IdentifyGame(std::string_view id) {
    return id == "eut2" ? GameKind::ETS2 : id == "ats" ? GameKind::ATS : GameKind::Unknown;
}

bool ResolveCatalogPools(const unsigned char* bytes, std::size_t size,
                         std::uint32_t& pools_rva, std::string& reason) {
    pools_rva = 0; reason.clear();
    Image image{bytes, size};
    if (!image.Parse()) { reason = "invalid PE image"; return false; }
    std::array<std::uint32_t, std::size(catalog162::signatures)> resolved{};
    std::vector<Reference> references, current;
    for (std::size_t n = 0; n < resolved.size(); ++n) {
        const auto& signature = catalog162::signatures[n];
        const auto& first = signature.instructions[0];
        unsigned matches = 0;
        for (const auto candidate : image.starts) {
            if (!image.Code(candidate, first.size) || std::memcmp(bytes + candidate, first.bytes, first.size)) continue;
            if (Match(image, signature, candidate, false, current)) {
                resolved[n] = candidate; references.insert(references.end(), current.begin(), current.end());
                if (++matches > 1) break;
            }
        }
        if (matches != 1) {
            reason = std::string(matches ? "ambiguous catalog signature: " : "incompatible catalog signature: ") + signature.name;
            return false;
        }
    }
    std::vector<std::uint32_t> symbols(catalog162::symbol_count, 0);
    for (const auto& ref : references) {
        if ((ref.callee >= 0 && ref.address != resolved[ref.callee]) ||
            (ref.symbol >= 0 && symbols[ref.symbol] && symbols[ref.symbol] != ref.address)) {
            reason = "inconsistent catalog reference graph"; return false;
        }
        if (ref.symbol >= 0) symbols[ref.symbol] = ref.address;
    }
    const auto pools = symbols[catalog162::pools_symbol];
    if (!pools || pools % 8 || !image.In(pools, 2048 * sizeof(std::uintptr_t),
                                       IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE, IMAGE_SCN_MEM_EXECUTE)) {
        reason = "invalid catalog token pools"; return false;
    }
    pools_rva = pools; return true;
}
bool ResolveCatalogNames(const unsigned char* bytes, std::size_t size, CatalogNameCode& out, std::string& reason) {
    out={}; reason.clear(); Image image{bytes,size};
    if (!image.Parse()) { reason="invalid name PE image"; return false; }
    std::vector<std::uint32_t> roots,symbols,core_roots,core_symbols;
    if (!ResolveSet(image,catalog_names162::signatures,std::size(catalog_names162::signatures),catalog_names162::symbol_count,roots,symbols,reason) ||
        !ResolveSet(image,catalog162::signatures,std::size(catalog162::signatures),catalog162::symbol_count,core_roots,core_symbols,reason)) return false;
    const auto table=symbols[catalog_names162::buffer_vtable_symbol];
    if (!image.In(table,32,IMAGE_SCN_MEM_READ,IMAGE_SCN_MEM_WRITE|IMAGE_SCN_MEM_EXECUTE)) { reason="invalid name buffer table"; return false; }
    const auto reserve=PointerRva(image,Read<std::uint64_t>(bytes+table+16));
    const auto clear=PointerRva(image,Read<std::uint64_t>(bytes+table+24));
    const auto dynamic=PointerRva(image,Read<std::uint64_t>(bytes+table+8));
    const unsigned char reserve_code[]={0xb8,0,8,0,0,0xc3};
    const unsigned char clear_code[]={0xc6,0x41,0x18,0,0xc3};
    const unsigned char dynamic_code[]={0x32,0xc0,0xc3};
    if (!image.Code(reserve,sizeof(reserve_code)) || std::memcmp(bytes+reserve,reserve_code,sizeof(reserve_code)) ||
        !image.Code(clear,sizeof(clear_code)) || std::memcmp(bytes+clear,clear_code,sizeof(clear_code)) ||
        !image.Code(dynamic,sizeof(dynamic_code)) || std::memcmp(bytes+dynamic,dynamic_code,sizeof(dynamic_code))) {
        reason="incompatible fixed name buffer"; return false;
    }
    const PropertyCheck cargo[]={{"name",0x10,1},{"icon",0x30,1},{"unit_reward_per_km",0x50,5},{"fragility",0x54,5},{"valuable",0x5c,0x39}};
    const PropertyCheck company[]={{"name",0x10,1},{"sort_name",0x30,1},{"parent_company",0x50,1}};
    const PropertyCheck city[]={{"city_name",0x10,1},{"city_name_localized",0x30,1},{"short_city_name",0x50,1}};
    if (!PropertyGroup(image,cargo,std::size(cargo)) || !PropertyGroup(image,company,std::size(company)) || !PropertyGroup(image,city,std::size(city))) {
        reason="incompatible display-name field metadata"; return false;
    }
    // The game's city labels select +18 or +38 using this shared locale preference.
    const unsigned char city_select[]={0x0f,0xb6,0x05,0,0,0,0,0x84,0xc0,0xb9,0x18,0,0,0,0x0f,0x45,0xca,0x4c,0x8b,0x1c,0x0f};
    std::uint32_t preference=0;
    for (const auto& section:image.sections) {
        if (!(section.flags&IMAGE_SCN_MEM_EXECUTE)) continue;
        for (std::uint64_t at=section.rva;at+sizeof(city_select)<=std::uint64_t(section.rva)+section.size;++at) {
            if (std::memcmp(bytes+at,city_select,3) || std::memcmp(bytes+at+7,city_select+7,sizeof(city_select)-7)) continue;
            const auto target=static_cast<std::int64_t>(at)+7+Read<std::int32_t>(bytes+at+3);
            if (target<0 || target>UINT32_MAX || !image.In(static_cast<std::uint32_t>(target),1,IMAGE_SCN_MEM_READ|IMAGE_SCN_MEM_WRITE,IMAGE_SCN_MEM_EXECUTE) ||
                (preference && preference!=target)) { reason="inconsistent city label preference"; return false; }
            preference=static_cast<std::uint32_t>(target);
        }
    }
    if (!preference) { reason="city label preference not found"; return false; }
    out={roots[0],table,core_roots[0],preference}; return true;
}
bool ResolveCreationCode(const unsigned char* bytes,std::size_t size,const GameProfile& core,CreationCode& out,std::string& reason) {
    out={};reason.clear(); Image image{bytes,size};
    if(!image.Parse() || core.schedule<9){reason="invalid creation image/profile";return false;}
    using namespace creation162;
    std::vector<std::uint32_t> resolved(std::size(signatures),0),symbols(symbol_count,0);
    resolved[generator_index]=core.generator;resolved[fill_index]=core.fill_company;
    resolved[clear_company_index]=core.clear_company;resolved[schedule_index]=core.schedule-9;
    resolved[refill_index]=core.refill;resolved[clear_vector_index]=core.clear_vector;
    std::vector<bool> checked(resolved.size(),false);
    std::vector<Reference> references;
    bool progress=true;
    while(progress){
        progress=false;
        for(std::size_t n=0;n<resolved.size();++n){
            if(!resolved[n] || checked[n])continue;
            if(!std::binary_search(image.starts.begin(),image.starts.end(),resolved[n]) ||
                !Match(image,signatures[n],resolved[n],false,references)){
                reason=std::string("incompatible creation function: ")+signatures[n].name;return false;
            }
            checked[n]=true;progress=true;
            for(const auto& ref:references){
                if(ref.callee>=0){
                    if(resolved[ref.callee] && resolved[ref.callee]!=ref.address){reason="conflicting creation function links";return false;}
                    resolved[ref.callee]=ref.address;
                }
                if(ref.symbol>=0){
                    if(symbols[ref.symbol] && symbols[ref.symbol]!=ref.address){reason="conflicting creation global links";return false;}
                    symbols[ref.symbol]=ref.address;
                }
            }
        }
    }
    if(std::find(checked.begin(),checked.end(),false)!=checked.end()){reason="incomplete creation graph";return false;}
    const auto index=symbols[allocator_index_symbol],array=symbols[allocator_array_symbol],prefix=symbols[destination_prefix_symbol];
    if(!image.In(index,4,IMAGE_SCN_MEM_READ|IMAGE_SCN_MEM_WRITE,IMAGE_SCN_MEM_EXECUTE) ||
        !image.In(array,4*8,IMAGE_SCN_MEM_READ|IMAGE_SCN_MEM_WRITE,IMAGE_SCN_MEM_EXECUTE) ||
        !image.In(prefix,8,IMAGE_SCN_MEM_READ,IMAGE_SCN_MEM_EXECUTE)){
        reason="invalid creation globals";return false;
    }
    out={resolved[owned_candidates_index],resolved[base_candidates_index],resolved[destinations_index],resolved[base_slot_index],
         resolved[base_factory_index],resolved[schedule_index],resolved[free_event_node_index],resolved[physical_update_index],index,array,prefix};
    return true;
}
bool ResolveDeliveryTimeCode(const unsigned char* bytes,std::size_t size,const CreationCode& creation,DeliveryTimeCode& out,std::string& reason){
    out={};reason.clear();Image image{bytes,size};if(!image.Parse()){reason="invalid delivery-time PE";return false;}
    using namespace delivery_time162;
    std::vector<std::uint32_t> roots,symbols;
    if(!ResolveSet(image,signatures,std::size(signatures),symbol_count,roots,symbols,reason))return false;
    if(roots[freight_factory_index]!=creation.base_factory){reason="delivery-time factory differs from creation factory";return false;}
    const auto freight=symbols[freight_vtable_symbol],owned=symbols[owned_vtable_symbol];
    for(const auto pair:{std::make_pair(freight,roots[freight_window_index]),std::make_pair(owned,roots[owned_window_index])}){
        if(!image.In(pair.first,0x70,IMAGE_SCN_MEM_READ,IMAGE_SCN_MEM_EXECUTE|IMAGE_SCN_MEM_WRITE)||
           PointerRva(image,Read<std::uint64_t>(bytes+pair.first+0x68))!=pair.second){reason="invalid truck delivery-time vtable";return false;}
    }
    const PropertyCheck limits[]={{"time_lower_limit",0x110,0x27},{"time_upper_limit",0x114,0x27}};
    if(!PropertyGroup(image,limits,std::size(limits))){reason="incompatible truck delivery-time metadata";return false;}
    out={roots[freight_window_index],roots[owned_window_index]};return true;
}
bool ResolveRoadTripCode(const unsigned char* bytes,std::size_t size,GameKind game,RoadTripCode& out,std::string& reason) {
    out={};reason.clear();
    if(game!=GameKind::ATS){reason="Road Trip is available only in ATS";return false;}
    Image image{bytes,size};if(!image.Parse()){reason="invalid Road Trip PE image";return false;}
    using namespace road_trip162;
    std::vector<std::uint32_t> roots(std::size(signatures)),symbols(symbol_count);
    std::vector<Reference> refs;
    // Discover only independent roots; other functions must be linked callees.
    for(const auto root:{stock_refresh_index,factory_index,refresh_initial_index,registry_source_index}){
        unsigned found=0;
        for(auto at:image.starts){
            const auto& first=signatures[root].instructions[0];
            if(!image.Code(at,first.size)||std::memcmp(bytes+at,first.bytes,first.width?first.offset:first.size))continue;
            if(Match(image,signatures[root],at,false,refs)){roots[root]=at;if(++found>1)break;}
        }
        if(found!=1){reason=std::string("incompatible Road Trip root: ")+signatures[root].name;return false;}
    }
    std::vector<bool> checked(roots.size());bool progress=true;
    while(progress){progress=false;for(unsigned n=0;n<roots.size();++n){
        if(!roots[n]||checked[n])continue;
        if(!std::binary_search(image.starts.begin(),image.starts.end(),roots[n])||!Match(image,signatures[n],roots[n],false,refs)){
            reason=std::string("incompatible Road Trip function: ")+signatures[n].name;return false;
        }
        checked[n]=true;progress=true;
        for(const auto& ref:refs){
            if(ref.callee>=0){if(roots[ref.callee]&&roots[ref.callee]!=ref.address){reason="Road Trip call graph conflict";return false;}roots[ref.callee]=ref.address;}
            if(ref.symbol>=0){if(symbols[ref.symbol]&&symbols[ref.symbol]!=ref.address){reason="Road Trip global conflict";return false;}symbols[ref.symbol]=ref.address;}
        }
    }}
    if(std::find(checked.begin(),checked.end(),false)!=checked.end()){reason="incomplete Road Trip graph";return false;}
    const PropertyCheck offer[]={{"source",0x10,1},{"target",0x48,1},{"expiration_time",0x80,0x27},{"urgency",0x84,0x27},
        {"job_type",0x8c,0x3b},{"shortest_distance_km",0x90,0x2b},{"ferry_time",0x92,0x2b},{"ferry_price",0x94,0x2b},
        {"job",0x98,0x41},{"cargo",0xa0,0x41},{"company_car",0xa8,1},{"units_count",0xc8,0x27}};
    const PropertyCheck job[]={{"cargos",0x38,0x40},{"vehicle_types",0x60,4},{"source_data_contexts",0x88,4},{"target_data_contexts",0xb0,4}};
    if(!PropertyGroup(image,offer,std::size(offer))||!PropertyGroup(image,job,std::size(job))){reason="incompatible Road Trip field metadata";return false;}
    const auto registry=symbols[registry_symbol],format=symbols[point_format_symbol];
    if(!image.In(registry,8,IMAGE_SCN_MEM_READ|IMAGE_SCN_MEM_WRITE,IMAGE_SCN_MEM_EXECUTE)||
       !image.In(format,6,IMAGE_SCN_MEM_READ,IMAGE_SCN_MEM_WRITE|IMAGE_SCN_MEM_EXECUTE)||std::memcmp(bytes+format,"%s.%s",6)){
        reason="invalid Road Trip globals";return false;
    }
    out={roots[generate_index],roots[factory_index],roots[select_car_index],roots[schedule_index],roots[remove_event_index],
         roots[format_text_index],roots[assign_link_index],roots[assign_text_index],registry,format,
         roots[lookup_car_index],symbols[vehicle_registry_symbol]};
    if(!image.In(out.vehicle_registry,32,IMAGE_SCN_MEM_READ,IMAGE_SCN_MEM_WRITE|IMAGE_SCN_MEM_EXECUTE)){out={};reason="invalid vehicle registry";return false;}
    return true;
}
bool ReadExecutableVersion(const wchar_t* path, ExecutableVersion& out) {
    out = {};
    const DWORD size = GetFileVersionInfoSizeW(path, nullptr);
    if (!size) return false;
    std::vector<unsigned char> bytes(size);
    if (!GetFileVersionInfoW(path, 0, size, bytes.data())) return false;
    VS_FIXEDFILEINFO* info = nullptr; UINT length = 0;
    if (!VerQueryValueW(bytes.data(), L"\\", reinterpret_cast<void**>(&info), &length) ||
        length < sizeof(*info) || info->dwSignature != 0xfeef04bd) return false;
    out = {HIWORD(info->dwFileVersionMS), LOWORD(info->dwFileVersionMS),
           HIWORD(info->dwFileVersionLS), LOWORD(info->dwFileVersionLS)};
    return true;
}

static bool ResolveWithLayout(const Image& image, const SignatureSet& layout, GameKind game,
                              GameProfile& out, std::string& reason) {
    out = {}; reason.clear();
    const auto* bytes = image.data;
    const auto size = image.size;
    std::vector<std::uint32_t> resolved(layout.count, 0);
    std::vector<Reference> references, current;
    for (unsigned s = 0; s < layout.count; ++s) {
        const auto& signature = layout.signatures[s];
        unsigned matches = 0;
        for (auto candidate : image.starts) {
            const auto& first = signature.instructions[0];
            if (!image.Code(candidate, first.size) ||
                std::memcmp(image.data + candidate, first.bytes, first.size)) continue;
            if (Match(image, signature, candidate, s >= 7, current)) {
                if (++matches > 1) break;
                resolved[s] = candidate;
                references.insert(references.end(), current.begin(), current.end());
            }
        }
        if (matches != 1) {
            reason = std::string(matches ? "ambiguous signature: " : "incompatible signature: ") + signature.name;
            return false;
        }
    }
    std::vector<std::uint32_t> symbols(layout.symbol_count, 0);
    for (const auto& ref : references) {
        if ((ref.callee >= 0 && ref.address != resolved[ref.callee]) ||
            (ref.symbol >= 0 && symbols[ref.symbol] && symbols[ref.symbol] != ref.address)) {
            reason = "inconsistent function/global reference graph"; return false;
        }
        if (ref.symbol >= 0) symbols[ref.symbol] = ref.address;
    }
    const auto source = resolved[7] + layout.economy_source_load;
    const auto economy64 = static_cast<std::int64_t>(source) + 7 + Read<std::int32_t>(bytes + source + 3);
    if (economy64 < 0 || economy64 > UINT32_MAX ||
        !image.In(static_cast<std::uint32_t>(economy64), sizeof(std::uintptr_t),
                  IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE, IMAGE_SCN_MEM_EXECUTE) || economy64 % 8) {
        reason = "invalid global economy location"; return false;
    }
    GameProfile profile{};
    profile.name = game == GameKind::ATS ? layout.ats_name : layout.ets2_name;
    profile.sha256 = L"";
    profile.economy = static_cast<std::uint32_t>(economy64);
    profile.clear_vector = resolved[0]; profile.fill_company = resolved[1]; profile.refill = resolved[2];
    profile.generator = resolved[3]; profile.clear_company = resolved[4]; profile.schedule = resolved[5] + 9;
    profile.clear_call = profile.generator + layout.generator_clear_call;
    profile.attempt_branch = profile.generator + layout.generator_budget_branch;
    profile.attempt_immediate = profile.generator + layout.generator_budget_immediate;
    const unsigned char budget[] = {0x73,0x0b,0x48,0xc7,0x44,0x24,
                                    static_cast<unsigned char>(layout.generator_budget_stack),0x04,0,0,0};
    const unsigned char schedule[] = {0x48,0x89,0x5c,0x24,0x10};
    if (!image.Code(profile.clear_call, 5) || bytes[profile.clear_call] != 0xe8 ||
        static_cast<std::int64_t>(profile.clear_call) + 5 + Read<std::int32_t>(bytes + profile.clear_call + 1) != profile.clear_vector ||
        !image.Code(profile.attempt_branch, sizeof(budget)) ||
        std::memcmp(bytes + profile.attempt_branch, budget, sizeof(budget)) ||
        !image.Code(profile.schedule, sizeof(schedule)) ||
        std::memcmp(bytes + profile.schedule, schedule, sizeof(schedule))) {
        reason = "incompatible hook/budget instruction layout"; return false;
    }
    // Literal patch guards are captured only after the complete instruction/reference validation.
    Guard(profile, 0, image, profile.generator, 25); Guard(profile, 1, image, profile.refill, 16);
    Guard(profile, 2, image, profile.clear_vector, 16); Guard(profile, 3, image, profile.clear_company, 16);
    Guard(profile, 4, image, profile.fill_company, 16); Guard(profile, 5, image, resolved[5], 14);
    Guard(profile, 6, image, profile.clear_call, 5); Guard(profile, 7, image, profile.attempt_branch, 2);
    Guard(profile, 8, image, profile.attempt_branch + 2, 9); Guard(profile, 9, image, source, 32);
    if (!ValidateGameProfile(profile, bytes, size)) { reason = "resolved patch guards failed"; return false; }
    out = profile;
    return true;
}

bool ResolveGameProfile(const unsigned char* bytes, std::size_t size, GameKind game,
                        GameProfile& out, std::string& reason) {
    out = {}; reason.clear();
    if (game != GameKind::ATS && game != GameKind::ETS2) {
        reason = "requires ATS or ETS2"; return false;
    }
    Image image{bytes, size};
    if (!image.Parse()) { reason = "invalid mapped x64 PE image/sections/function table"; return false; }
    GameProfile selected{};
    unsigned matches = 0;
    for (const auto& layout : layouts) {
        GameProfile candidate{}; std::string failure;
        if (ResolveWithLayout(image, layout, game, candidate, failure)) {
            selected = candidate;
            ++matches;
        } else {
            if (!reason.empty()) reason += "; ";
            reason += std::string(layout.name) + ": " + failure;
        }
    }
    if (matches != 1) {
        if (matches > 1) reason = "ambiguous compatible code layouts";
        return false;
    }
    out = selected; reason.clear();
    return true;
}
