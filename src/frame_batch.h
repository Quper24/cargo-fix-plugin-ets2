#pragma once
#include <cstdint>
#include <vector>
#include <algorithm>
// Native game calls stay on the game thread; yield between bounded groups.
class FrameBatch {
public:
    void Begin(const std::uintptr_t* data,std::size_t count){Cancel();if(data && count){items.assign(data,data+count);active=true;}}
    void Cancel(){items.clear();cursor=0;active=false;}
    bool Active() const{return active;}
    std::size_t Done() const{return cursor;}
    std::size_t Total() const{return items.size();}
    template<class Process,class Clock> void Step(Process process,Clock clock){
        if(!active)return;
        const auto started=clock();unsigned count=0;
        do{const auto index=cursor++;process(items[index],index);++count;}
        while(cursor<items.size() && count<16 && clock()-started<8);
        active=cursor<items.size();
    }
private:
    std::vector<std::uintptr_t> items;
    std::size_t cursor=0;
    bool active=false;
};
