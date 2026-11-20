#include "game_resolver.h"
#include <Windows.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

#define Check(value) do { if (!(value)) { std::fprintf(stderr, "Resolver check failed: line %d\n", __LINE__); std::abort(); } } while (0)

int main(int argc, char** argv) {
    Check(IdentifyGame("ats") == GameKind::ATS);
    Check(IdentifyGame("eut2") == GameKind::ETS2);
    Check(IdentifyGame("other") == GameKind::Unknown);
    GameProfile profile{}; std::string reason;
    Check(!ResolveGameProfile(nullptr, 0, GameKind::ATS, profile, reason));
    Check(!reason.empty() && !profile.generator);
    if (argc < 2) return 0;
    const auto path = std::filesystem::path(argv[1]);
    std::ifstream file(path, std::ios::binary);
    Check(file.good());
    std::vector<unsigned char> raw((std::istreambuf_iterator<char>(file)), {});
    const bool mapped = argc == 3 && std::strcmp(argv[2], "--mapped-reference") == 0;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(raw.data());
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(raw.data() + dos->e_lfanew);
    std::vector<unsigned char> image(nt->OptionalHeader.SizeOfImage);
    if (mapped) {
        // The Ghidra export is padded to 64 MiB; only SizeOfImage belongs to the PE.
        Check(raw.size() >= image.size());
        std::copy_n(raw.data(), image.size(), image.data());
    }
    else std::copy_n(raw.data(), nt->OptionalHeader.SizeOfHeaders, image.data());
    const auto* sections = IMAGE_FIRST_SECTION(nt);
    for (unsigned n = 0; !mapped && n < nt->FileHeader.NumberOfSections; ++n) {
        const auto& s = sections[n];
        Check(std::uint64_t(s.PointerToRawData) + s.SizeOfRawData <= raw.size());
        Check(std::uint64_t(s.VirtualAddress) + s.SizeOfRawData <= image.size());
        std::copy_n(raw.data() + s.PointerToRawData, s.SizeOfRawData, image.data() + s.VirtualAddress);
    }
    const auto kind = path.filename() == L"amtrucks.exe" ? GameKind::ATS : GameKind::ETS2;
    if (!mapped) {
        ExecutableVersion file_version{};
        Check(ReadExecutableVersion(path.c_str(), file_version));
        Check(file_version.major == 1 && (file_version.minor == 61 || file_version.minor == 62));
    }
    if (!ResolveGameProfile(image.data(), image.size(), kind, profile, reason)) {
        std::fprintf(stderr, "%s: %s\n", argv[1], reason.c_str()); return 1;
    }
    Check(ValidateGameProfile(profile, image.data(), image.size()));
    // Compare the dynamic result against independently saved address fixtures.
    const auto expected = mapped ? &GameProfiles()[0] :
        path.parent_path().filename() == L"temporary_1_61-1.61.1.1" ? &GameProfiles()[1] :
        path.parent_path().filename() == L"oculus-1.61.1.1009" ? &GameProfiles()[2] :
        path.parent_path().filename() == L"public_beta-1.62.0.10" ? &GameProfiles()[3] : nullptr;
    if (expected) {
        Check(profile.economy == expected->economy && profile.generator == expected->generator &&
              profile.refill == expected->refill && profile.clear_vector == expected->clear_vector &&
              profile.clear_company == expected->clear_company && profile.fill_company == expected->fill_company &&
              profile.schedule == expected->schedule && profile.clear_call == expected->clear_call &&
              profile.attempt_branch == expected->attempt_branch && profile.attempt_immediate == expected->attempt_immediate);
    }
    std::printf("Resolved %s: economy=%x generator=%x clear=%x fill=%x schedule=%x\n",
        profile.name, profile.economy, profile.generator, profile.clear_company, profile.fill_company, profile.schedule);
    auto attempt = [&](GameKind game, std::size_t size) {
        GameProfile result{};
        return ResolveGameProfile(image.data(), size, game, result, reason);
    };
    Check(!attempt(GameKind::Unknown, image.size()));
    Check(!attempt(kind, image.size()-1));
    // Mutations inside the six function bodies must fail, including ABI offsets beyond short prologues.
    for (auto rva : {profile.generator+0x62, profile.refill+12, profile.clear_vector+20,
                     profile.clear_company+30, profile.fill_company+11, profile.schedule+20,
                     profile.clear_call+1, profile.attempt_branch, profile.attempt_branch+1, profile.attempt_immediate,
                     profile.guards[9].rva+3}) {
        image[rva] ^= 1;
        Check(!attempt(kind, image.size()));
        image[rva] ^= 1;
    }
    // An unrelated metadata change (and hence a different file hash) is accepted.
    auto* mapped_nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(image.data() + dos->e_lfanew);
    mapped_nt->FileHeader.TimeDateStamp ^= 0x12345678;
    Check(attempt(kind, image.size()));
    mapped_nt->FileHeader.TimeDateStamp ^= 0x12345678;

    // Duplicate a complete candidate at another real function entry: ambiguity must fail closed.
    constexpr unsigned vector_length = 0x9a;
    std::vector<unsigned char> backup(image.begin()+profile.generator, image.begin()+profile.generator+vector_length);
    std::copy_n(image.data()+profile.clear_vector, vector_length, image.data()+profile.generator);
    Check(!attempt(kind, image.size()));
    Check(reason.find("ambiguous signature: clear_vector") != std::string::npos);
    std::copy(backup.begin(), backup.end(), image.begin()+profile.generator);

    // Relocate every section/RVA while keeping the instruction and reference graph intact.
    // This is a synthetic future layout, not one of the known executable address tables.
    constexpr DWORD shift = 0x10000;
    std::vector<unsigned char> relocated(image.size()+shift);
    std::copy_n(image.data(), nt->OptionalHeader.SizeOfHeaders, relocated.data());
    auto* relocated_nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(relocated.data()+dos->e_lfanew);
    auto* relocated_sections = IMAGE_FIRST_SECTION(relocated_nt);
    for (unsigned n = 0; n < nt->FileHeader.NumberOfSections; ++n) {
        const auto& section = sections[n];
        const auto length = std::max(section.Misc.VirtualSize, section.SizeOfRawData);
        std::copy_n(image.data()+section.VirtualAddress, length, relocated.data()+section.VirtualAddress+shift);
        relocated_sections[n].VirtualAddress += shift;
    }
    relocated_nt->OptionalHeader.SizeOfImage += shift;
    relocated_nt->OptionalHeader.AddressOfEntryPoint += shift;
    auto& directory = relocated_nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    directory.VirtualAddress += shift;
    auto* functions = reinterpret_cast<RUNTIME_FUNCTION*>(relocated.data()+directory.VirtualAddress);
    for (unsigned n = 0; n < directory.Size/sizeof(*functions); ++n) {
        functions[n].BeginAddress += shift; functions[n].EndAddress += shift; functions[n].UnwindData += shift;
    }
    GameProfile moved{};
    Check(ResolveGameProfile(relocated.data(), relocated.size(), kind, moved, reason));
    Check(moved.generator == profile.generator+shift && moved.economy == profile.economy+shift &&
          moved.refill == profile.refill+shift && moved.clear_vector == profile.clear_vector+shift &&
          moved.clear_company == profile.clear_company+shift && moved.fill_company == profile.fill_company+shift &&
          moved.schedule == profile.schedule+shift);
    // Change the actual VS_FIXEDFILEINFO resource, not a separate version parameter.
    // These are synthetic updates of preserved code; no claim to have a real future EXE.
    unsigned version_resources = 0;
    for (unsigned s = 0; s < nt->FileHeader.NumberOfSections; ++s) {
        if (std::memcmp(sections[s].Name, ".rsrc", 5)) continue;
        const auto begin = sections[s].VirtualAddress;
        const auto end = begin + sections[s].Misc.VirtualSize;
        for (auto pos = begin; pos + sizeof(VS_FIXEDFILEINFO) <= end; ++pos) {
            VS_FIXEDFILEINFO info{}; std::memcpy(&info, image.data()+pos, sizeof(info));
            if (info.dwSignature != 0xfeef04bd || HIWORD(info.dwFileVersionMS) != 1 ||
                (LOWORD(info.dwFileVersionMS) != 61 && LOWORD(info.dwFileVersionMS) != 62)) continue;
            ++version_resources;
            const auto original = info;
            for (const auto& update : {ExecutableVersion{1,61,2,0}, ExecutableVersion{1,62,999,65535}, ExecutableVersion{1,63,0,0}}) {
                info.dwFileVersionMS = info.dwProductVersionMS = MAKELONG(update.minor, update.major);
                info.dwFileVersionLS = info.dwProductVersionLS = MAKELONG(update.build, update.patch);
                std::memcpy(image.data()+pos, &info, sizeof(info));
                Check(attempt(kind, image.size()));
            }
            std::memcpy(image.data()+pos, &original, sizeof(original));
        }
    }
    Check(version_resources || mapped);
    std::puts("Version-resource/metadata/RVA changes accepted; field/call/branch mutations, ambiguity and truncation rejected.");
}
