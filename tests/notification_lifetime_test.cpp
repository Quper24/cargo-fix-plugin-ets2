#include "notification_lifetime.h"
#include <cstdlib>
void Check(bool ok) { if (!ok) std::abort(); }
int main() {
    NotificationLifetime life;
    Check(!life.Tick(0,true));
    life.Start(); Check(life.Tick(0,true));
    Check(life.Tick(60000,true)); // A minute-long generation stall uses at most one frame budget.
    Check(life.Tick(61000,false));
    Check(life.Tick(120000,false)); // Hidden time is not reading time.
    Check(life.Tick(180000,true)); // Returning to game preserves the remaining budget.
    for(unsigned i=1;i<=26;++i) Check(life.Tick(180000+i*250,true));
    Check(!life.Tick(186750,true));
    life.Start(); // A result replaces the progress message and receives its own full budget.
    Check(life.Tick(200000,true));
    for(unsigned i=1;i<28;++i) Check(life.Tick(200000+i*250,true));
    Check(!life.Tick(207000,true));
}
