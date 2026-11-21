#include "delivery_time_policy.h"
#include <cstdlib>
void Check(bool condition){if(!condition)std::abort();}
int main(){
    using namespace delivery_time;
    std::uint32_t result=123;
    Check(Quadruple(10000,10360,result)&&result==11440); // Six hours become 24, clock remains unchanged.
    Check(Quadruple(10000,10001,result)&&result==10004);
    Check(Quadruple(0,UINT32_MAX/4,result)&&result==4*(UINT32_MAX/4));
    result=123;Check(!Quadruple(10000,9999,result)&&result==123);
    Check(!Quadruple(10000,10000,result)&&result==123);
    Check(!Quadruple(UINT32_MAX-3,UINT32_MAX,result)&&result==123);
    Fingerprint a{"cargo.salt","company.target.city",12000,10,1,300};
    auto b=a;Check(a==b);++b.expiry;Check(!(a==b));
    b=a;b.cargo="cargo.other";Check(!(a==b));
    Record first{"company.source.city",false,2,a},other=first;
    Check(SameSlot(first,other));other.owned=true;Check(!SameSlot(first,other));
}
