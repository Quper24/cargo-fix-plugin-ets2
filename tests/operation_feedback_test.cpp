#include "operation_feedback.h"
#include <cstdlib>
void Check(bool value){if(!value)std::abort();}
int main(){
    overlay_ui::OperationFeedback state;
    Check(!state.Active() && !state.Take());
    Check(!state.Request(0) && !state.Request(5));
    for(int action=1;action<=4;++action){
        Check(state.Request(action));Check(state.Active()==action);
        Check(!state.Take());Check(!state.Request(action==1?2:1));
        state.Presented();Check(state.Take()==action);Check(!state.Take());
        Check(state.Active()==action && !state.Request(action));
        state.Complete();Check(!state.Active());
    }
    Check(state.Request(3));state.Complete();state.Presented();Check(!state.Take());
    Check(state.Request(4));Check(state.Take(true)==4); // Reject stale/unready profiles before rendering too.
    state.Complete();Check(!state.Take());
}
