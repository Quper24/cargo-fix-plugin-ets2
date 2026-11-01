#pragma once
#include <cstdint>
class RefreshCooldown {
public:
    void Start(std::uint64_t now) { started_ = now; active_ = true; }
    void Reset() { active_ = false; }
    std::uint64_t Remaining(std::uint64_t now) const {
        const auto elapsed = now - started_;
        return active_ && elapsed < 5000 ? 5000 - elapsed : 0;
    }
private:
    std::uint64_t started_ = 0;
    bool active_ = false;
};
