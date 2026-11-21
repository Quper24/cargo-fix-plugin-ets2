#include "frame_batch.h"
#include <cstdlib>
void Check(bool ok){if(!ok)std::abort();}
int main(){
    FrameBatch batch;std::vector<std::uintptr_t> input(100);
    for(unsigned i=0;i<input.size();++i)input[i]=i+1;
    batch.Begin(input.data(),input.size());input[0]=999;
    std::uint64_t clock=0;unsigned calls=0;
    auto process=[&](std::uintptr_t value,std::size_t index){Check(value==index+1);++calls;clock+=3;};
    batch.Step(process,[&]{return clock;});Check(calls==3 && batch.Done()==3 && batch.Active());
    while(batch.Active())batch.Step(process,[&]{return clock;});
    Check(calls==100 && batch.Done()==batch.Total());
    batch.Begin(input.data(),input.size());calls=0;
    batch.Step([&](auto,auto){++calls;},[]{return 0;});Check(calls==16 && batch.Active());
    batch.Cancel();batch.Step([&](auto,auto){std::abort();},[]{return 0;});
    Check(!batch.Active() && !batch.Total());
    batch.Begin(nullptr,0);Check(!batch.Active());
}
