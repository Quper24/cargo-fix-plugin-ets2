#pragma once
#ifndef DIRECTINPUT_VERSION
#define DIRECTINPUT_VERSION 0x0800
#endif
#include <Windows.h>
#include <dinput.h>
#include <algorithm>
#include <array>
#include <cstring>
class MenuPointer {
public:
    float x=0, y=0, wheel=0;
    std::array<bool,5> buttons{};
    void Begin(float width,float height) { width_=width;height_=height;x=width/2;y=height/2;wheel=0;buttons.fill(false); }
    void Resize(float width,float height) { width_=width;height_=height;Move(0,0); }
    void Move(float dx,float dy) { x=std::clamp(x+dx,0.f,std::max(0.f,width_-1)); y=std::clamp(y+dy,0.f,std::max(0.f,height_-1)); }
    bool State(void* bytes,DWORD size,bool accumulate=true) {
        if(!bytes || (size!=sizeof(DIMOUSESTATE) && size!=sizeof(DIMOUSESTATE2)))return false;
        DIMOUSESTATE2 state{};std::memcpy(&state,bytes,size);
        if(accumulate) { Move(float(state.lX),float(state.lY));wheel+=float(state.lZ)/WHEEL_DELTA;
            for(unsigned i=0;i<buttons.size();++i)buttons[i]=(state.rgbButtons[i]&0x80)!=0;
        }
        std::memset(bytes,0,size);return true;
    }
    void Event(DIDEVICEOBJECTDATA& event,bool accumulate=true) {
        if(accumulate) {
            if(event.dwOfs==DIMOFS_X)Move(float(static_cast<LONG>(event.dwData)),0);
            else if(event.dwOfs==DIMOFS_Y)Move(0,float(static_cast<LONG>(event.dwData)));
            else if(event.dwOfs==DIMOFS_Z)wheel+=float(static_cast<LONG>(event.dwData))/WHEEL_DELTA;
            else if(event.dwOfs>=DIMOFS_BUTTON0 && event.dwOfs<DIMOFS_BUTTON0+buttons.size())buttons[event.dwOfs-DIMOFS_BUTTON0]=(event.dwData&0x80)!=0;
        }
        event.dwData=0;
    }
private:
    float width_=1,height_=1;
};
