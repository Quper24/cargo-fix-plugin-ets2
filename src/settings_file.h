#pragma once
#include <filesystem>

// Read old shortcuts until the first save to CargoControl.ini. Keep the old file intact.
inline std::filesystem::path SettingsReadPath(const std::filesystem::path& directory) {
    std::error_code error;
    const auto current = directory / L"CargoControl.ini";
    if (std::filesystem::exists(current, error)) return current;
    const auto legacy = directory / L"CargoControlETS2.ini";
    if (std::filesystem::exists(legacy, error)) return legacy;
    return current;
}
