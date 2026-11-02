#pragma once
#include <cstdint>
namespace hotkeys {
constexpr std::uint16_t Menu = 0x0678; // Ctrl + Alt + F9, Win32 HOTKEY flags.
constexpr std::uint16_t Valid(unsigned value) {
    const auto key = value & 255u, mods = value >> 8;
    return value <= 65535 && key && !(mods & ~15u) &&
        key != 0x10 && key != 0x11 && key != 0x12 && key != 0x5b && key != 0x5c &&
        !(key >= 0xa0 && key <= 0xa5) && key != 255 ? static_cast<std::uint16_t>(value & 0x7ffu) : 0;
}
constexpr bool PairAllowed(std::uint16_t a, std::uint16_t b) {
    return a != Menu && b != Menu && (!a || a != b);
}
constexpr bool Matches(std::uint16_t binding, bool key, bool ctrl, bool shift, bool alt) {
    return (binding & 255) && key && ctrl == !!(binding & 0x200) &&
        shift == !!(binding & 0x100) && alt == !!(binding & 0x400);
}
}
