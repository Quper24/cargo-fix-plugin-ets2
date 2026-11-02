#pragma once
#include <cstdint>
#include <algorithm>
// Count only visible, regularly rendered frame intervals. Loading/generation stalls
// and focus loss must not consume the user's reading time.
class NotificationLifetime {
public:
    void Start() { remaining_ = 7000; previous_visible_ = false; }
    bool Tick(std::uint64_t now, bool visible) {
        if (previous_visible_ && visible) {
            const auto delta = std::min<std::uint64_t>(now - previous_, 250);
            remaining_ = delta >= remaining_ ? 0 : remaining_ - delta;
        }
        previous_ = now; previous_visible_ = visible;
        return remaining_ != 0;
    }
private:
    std::uint64_t remaining_ = 0, previous_ = 0;
    bool previous_visible_ = false;
};
