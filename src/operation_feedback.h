#pragma once
namespace overlay_ui {
// Caller serializes access. Start only after the feedback reached a rendered frame.
class OperationFeedback {
public:
    bool Request(int action){
        if(active || action<1 || action>4)return false;
        active=action;pending=true;presented=false;return true;
    }
    int Take(bool reject_unpresented=false){if(!pending || (!presented && !reject_unpresented))return 0;pending=false;return active;}
    void Presented(){if(active)presented=true;}
    void Complete(){active=0;pending=presented=false;}
    int Active() const{return active;}
private:
    int active=0;
    bool pending=false,presented=false;
};
}
