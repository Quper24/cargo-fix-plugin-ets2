#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

struct CodeGuard {
    std::uint32_t rva;
    std::uint32_t length;
    std::array<unsigned char, 32> bytes;
};

struct GameProfile {
    const char* name;
    const wchar_t* sha256;
    std::uint32_t economy, generator, refill, clear_vector;
    std::uint32_t clear_company, fill_company, schedule;
    std::uint32_t clear_call, attempt_branch, attempt_immediate;
    std::array<CodeGuard, 10> guards;
};

const std::array<GameProfile, 4>& GameProfiles();
const GameProfile* FindGameProfile(std::wstring_view sha256);
// All checks run before writing any game code. Also used by the offline EXE check.
bool ValidateGameProfile(const GameProfile& profile, const unsigned char* image,
                         std::size_t image_size);
