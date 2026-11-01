#include "generation_limit.h"
#include "refresh_cooldown.h"
#include <cstdlib>
#include <iostream>
void Require(bool value) {
    if (!value) { std::cerr << "Generation limit check failed\n"; std::exit(1); }
}
int main() {
    GenerationLimit limit;
    Require(limit.RemainingUses() == 3);
    limit.Refresh(100);
    Require(limit.RemainingUses() == 3); // No successful generation: no consumption.
    limit.RecordSuccess(100);
    Require(limit.Deadline(100) == 2980);
    limit.RecordSuccess(500);
    limit.RecordSuccess(1500);
    Require(limit.Blocked() && limit.NextAvailable() == 2980);
    for (int paused_frame = 0; paused_frame < 100; ++paused_frame) limit.Refresh(1500);
    Require(limit.Blocked());
    Require(!limit.Refresh(2979) && limit.Blocked());
    Require(limit.Refresh(2980) && limit.RemainingUses() == 1);
    Require(limit.NextAvailable() == 3380);
    limit.RecordSuccess(2980);
    Require(limit.Blocked());
    Require(limit.Refresh(3380) && limit.RemainingUses() == 1);
    Require(limit.NextAvailable() == 4380);
    limit.Refresh(4000);
    limit.RecordSuccess(4000); // Fast travel / game-time advancement.
    Require(limit.Deadline(4000) == 6880);
    Require(limit.Refresh(4380) && limit.RemainingUses() == 1);
    Require(limit.Refresh(6880) && limit.RemainingUses() == 3);
    limit.Reset(); // Save-load signal / new session.
    Require(!limit.Blocked() && limit.RemainingUses() == 3);
    limit.RecordSuccess(50); // Loading an older save.
    Require(limit.Deadline(50) == 2930);
    RefreshCooldown freight;
    Require(freight.Remaining(0) == 0);
    freight.Start(0);
    Require(freight.Remaining(0) == 5000);
    Require(freight.Remaining(4999) == 1);
    Require(freight.Remaining(5000) == 0);
    for (unsigned i = 1; i <= 10; ++i) {
        freight.Start(i * 5000);
        Require(freight.Remaining(i * 5000 + 5000) == 0);
    }
    Require(limit.RemainingUses() == 2);
    freight.Start(60000);
    freight.Reset();
    Require(freight.Remaining(60001) == 0);
    std::cout << "Generation limit checks passed\n";
}
