#include "game_profile.h"
#include <Windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

void Require(bool value) { if (!value) std::abort(); }

int main(int argc, char** argv) {
    Require(FindGameProfile(L"unknown") == nullptr);
    for (const auto& p : GameProfiles()) Require(FindGameProfile(p.sha256) == &p);
    if (argc < 2) return 0;
    std::ifstream file(std::filesystem::path(argv[1]), std::ios::binary);
    Require(file.good());
    std::vector<unsigned char> raw((std::istreambuf_iterator<char>(file)), {});
    const bool mapped = argc == 3 && std::strcmp(argv[2], "--mapped-reference") == 0;
    const GameProfile* selected = &GameProfiles()[0];
    if (!mapped) {
        BCRYPT_ALG_HANDLE algorithm = nullptr;
        Require(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0);
        unsigned char digest[32]{};
        Require(BCryptHash(algorithm, nullptr, 0, raw.data(), static_cast<ULONG>(raw.size()), digest, 32) >= 0);
        BCryptCloseAlgorithmProvider(algorithm, 0);
        std::wstring hash;
        for (auto b : digest) { hash += L"0123456789ABCDEF"[b >> 4]; hash += L"0123456789ABCDEF"[b & 15]; }
        selected = FindGameProfile(hash);
        Require(selected != nullptr);
    }
    Require(raw.size() > sizeof(IMAGE_DOS_HEADER));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(raw.data());
    Require(dos->e_magic == IMAGE_DOS_SIGNATURE && dos->e_lfanew > 0);
    Require(static_cast<std::size_t>(dos->e_lfanew) + sizeof(IMAGE_NT_HEADERS64) <= raw.size());
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(raw.data() + dos->e_lfanew);
    Require(nt->Signature == IMAGE_NT_SIGNATURE);
    std::vector<unsigned char> image;
    if (mapped) image = raw;
    else {
        image.resize(nt->OptionalHeader.SizeOfImage);
        Require(nt->OptionalHeader.SizeOfHeaders <= raw.size());
        std::copy_n(raw.data(), nt->OptionalHeader.SizeOfHeaders, image.data());
        const auto* sections = IMAGE_FIRST_SECTION(nt);
        Require(reinterpret_cast<const unsigned char*>(sections + nt->FileHeader.NumberOfSections) <= raw.data() + raw.size());
        for (unsigned n = 0; n < nt->FileHeader.NumberOfSections; ++n) {
            const auto& s = sections[n];
            Require(static_cast<std::size_t>(s.PointerToRawData) + s.SizeOfRawData <= raw.size());
            Require(static_cast<std::size_t>(s.VirtualAddress) + s.SizeOfRawData <= image.size());
            std::copy_n(raw.data() + s.PointerToRawData, s.SizeOfRawData, image.data() + s.VirtualAddress);
        }
    }
    Require(ValidateGameProfile(*selected, image.data(), image.size()));
    for (const auto& p : GameProfiles())
        if (&p != selected) Require(!ValidateGameProfile(p, image.data(), image.size()));
    Require(!ValidateGameProfile(*selected, image.data(), selected->economy));
    for (const auto& guard : selected->guards) {
        image[guard.rva] ^= 1;
        Require(!ValidateGameProfile(*selected, image.data(), image.size()));
        image[guard.rva] ^= 1;
    }
    Require(ValidateGameProfile(*selected, image.data(), image.size()));
    std::int32_t relative = 0;
    std::memcpy(&relative, image.data() + selected->clear_call + 1, 4);
    Require(selected->clear_call + 5 + relative == selected->clear_vector);
    std::printf("Validated %s: executable identity, ten guards, wrong profiles, truncation and modified code rejection.\n", selected->name);
}
