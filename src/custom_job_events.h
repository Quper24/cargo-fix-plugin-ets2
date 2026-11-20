#pragma once
#include "custom_job_catalog.h"

namespace custom_jobs::creation {
struct EventNode {std::uintptr_t node=0;std::uint32_t time=0;};
using WritableCheck=std::function<bool(std::uintptr_t,std::size_t)>;
using Writer=std::function<void(std::uintptr_t,const void*,std::size_t)>;
inline bool CollectSlotEvents(const Reader& read,const WritableCheck& writable,std::uintptr_t scheduler,
                              std::uintptr_t company,std::uint32_t slot,std::vector<EventNode>& result){
    result.clear();
    if(!scheduler || scheduler>UINTPTR_MAX-0x60 || !writable(scheduler+0x40,24))return false;
    const auto sentinel=scheduler+0x48;
    std::uintptr_t node=0,previous=sentinel;
    std::uint64_t count=0,visited=0;
    if(!Read(read,scheduler+0x40,count) || count>1000000 || !Read(read,sentinel,node))return false;
    while(node!=sentinel){
        if(++visited>count || node>UINTPTR_MAX-24 || !writable(node,24))return false;
        std::uintptr_t before=0,next=0,event=0,owner=0;
        std::uint32_t event_slot=0,time=0;
        if(!Read(read,node,next)||!Read(read,node+8,before)||before!=previous||!Read(read,node+16,event)||!event||event>UINTPTR_MAX-0x28||
           !Read(read,event+0x18,owner)||!Read(read,event+0x20,event_slot)||!Read(read,event+0x10,time))return false;
        if(owner==company && event_slot==slot)result.push_back({node,time});
        previous=node;node=next;
    }
    std::uintptr_t last=0;
    return visited==count && Read(read,sentinel+8,last) && last==previous;
}
inline bool UnlinkSlotEvent(const Reader& read,const WritableCheck& writable,const Writer& write,
                            std::uintptr_t scheduler,std::uintptr_t node){
    if(!scheduler || scheduler>UINTPTR_MAX-0x60 || !node || node>UINTPTR_MAX-24 || node==scheduler+0x48)return false;
    std::uintptr_t next=0,previous=0,back=0,forward=0;
    std::uint64_t count=0;
    if(!Read(read,node,next)||!Read(read,node+8,previous)||!Read(read,scheduler+0x40,count)||!count||count>1000000||
       next>UINTPTR_MAX-16||!writable(next,16)||!writable(previous,8)||!writable(scheduler+0x40,8)||
       !Read(read,next+8,back)||back!=node||!Read(read,previous,forward)||forward!=node)return false;
    write(previous,&next,sizeof(next));write(next+8,&previous,sizeof(previous));
    --count;write(scheduler+0x40,&count,sizeof(count));return true;
}
}
