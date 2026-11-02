#include "game_profile.h"
#include <cstring>

namespace {
const std::array<GameProfile, 4> profiles = {{
#include "game_profile_data.inc"
}};
}

const std::array<GameProfile, 4>& GameProfiles() { return profiles; }

const GameProfile* FindGameProfile(std::wstring_view hash) {
    for (const auto& profile : profiles)
        if (hash == profile.sha256) return &profile;
    return nullptr;
}

bool ValidateGameProfile(const GameProfile& profile, const unsigned char* image,
                         std::size_t size) {
    if (!image || profile.economy > size || size - profile.economy < sizeof(std::uintptr_t))
        return false;
    for (const auto& guard : profile.guards) {
        if (!guard.length || guard.length > guard.bytes.size() ||
            guard.rva > size || guard.length > size - guard.rva ||
            std::memcmp(image + guard.rva, guard.bytes.data(), guard.length)) return false;
    }
    return true;
}
