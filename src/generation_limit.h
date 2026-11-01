#pragma once
#include <cstdint>
#include <array>
#include <algorithm>

class GenerationLimit {
public:
    static constexpr std::uint32_t PeriodMinutes = 48u * 60u;
    bool Refresh(std::uint32_t game_minute) {
        const auto previous = used_;
        unsigned retained = 0;
        for (unsigned i = 0; i < used_; ++i)
            if (expiries_[i] > game_minute) expiries_[retained++] = expiries_[i];
        used_ = retained;
        return previous != used_;
    }
    void Reset() { used_ = 0; expiries_.fill(0); }
    bool Blocked() const { return used_ >= 3; }
    unsigned Used() const { return used_; }
    unsigned RemainingUses() const { return 3 - used_; }
    std::uint32_t Deadline(std::uint32_t game_minute) const {
        return game_minute + PeriodMinutes;
    }
    std::uint32_t NextAvailable() const {
        return used_ ? *std::min_element(expiries_.begin(), expiries_.begin() + used_) : 0;
    }
    void RecordSuccess(std::uint32_t game_minute) {
        Refresh(game_minute);
        if (Blocked()) return;
        expiries_[used_++] = game_minute + PeriodMinutes;
    }
private:
    unsigned used_ = 0;
    std::array<std::uint32_t, 3> expiries_{};
};
