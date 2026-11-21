#pragma once
#include <cstdint>
#include <limits>
namespace overlay_ui {
// Caller serializes access. Start only after the feedback reached a rendered frame.
class OperationFeedback {
public:
    bool Request(int action,std::uint64_t now=0){
        if(active || action<1 || action>7)return false;
        active=action;pending=true;presents=0;started=now;return true;
    }
    int Take(bool reject_unpresented=false,std::uint64_t now=UINT64_MAX){if(!pending || (!reject_unpresented && (presents<3 || now<started || now-started<150)))return 0;pending=false;return active;}
    void Presented(){if(active && presents<3)++presents;}
    void Complete(){active=0;pending=false;presents=0;started=0;}
    int Active() const{return active;}
private:
    int active=0;
    bool pending=false;
    unsigned presents=0;
    std::uint64_t started=0;
};
}
