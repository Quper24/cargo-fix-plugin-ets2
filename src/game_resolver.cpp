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
}

GameKind IdentifyGame(std::string_view id) {
    return id == "eut2" ? GameKind::ETS2 : id == "ats" ? GameKind::ATS : GameKind::Unknown;
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
