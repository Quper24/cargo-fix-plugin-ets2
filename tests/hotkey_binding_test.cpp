#include "hotkey_binding.h"
#include <cstdlib>
void Check(bool ok) { if (!ok) std::abort(); }
int main() {
    using namespace hotkeys;
    Check(!Valid(0) && !Valid(0x10000) && !Valid(0x1000 + 0x77));
    Check(!Valid(0x11) && !Valid(0x5b) && !Valid(0xa3));
    Check(Valid(0xb77) == 0x377 && Valid(0xba) == 0xba);
    Check(Valid(0x377) == 0x377); // Ctrl+Shift+F8.
    Check(PairAllowed(0,0) && PairAllowed(0x377,0x378));
    Check(!PairAllowed(0x377,0x377));
    Check(!PairAllowed(Menu,0) && !PairAllowed(0,Menu));
    Check(!Matches(0,true,false,false,false));
    Check(Matches(0x377,true,true,true,false));
    Check(!Matches(0x377,true,true,true,true)); // Extra Alt does not trigger.
    Check(!Matches(0x377,true,false,true,false));
    Check(!Matches(0x377,false,true,true,false));
    Check(Matches(0x77,true,false,false,false));
    Check(!Matches(0x77,true,true,false,false));
}
