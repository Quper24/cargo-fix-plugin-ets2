#include "owned_offers.h"
#include <cstdlib>
#include <cstdio>

#define Check(value) do { if (!(value)) { std::fprintf(stderr, "Owned offers check failed: line %d\n", __LINE__); std::abort(); } } while (0)
extern "C" std::uint64_t ProbeBudgetStub(void* code, unsigned slot, unsigned carry);

struct alignas(8) Offer {
    std::array<unsigned char, 0x60> bytes{};
    Offer(const char* destination, std::uintptr_t cargo) {
        const auto text = reinterpret_cast<std::uintptr_t>(destination);
        const auto length = static_cast<std::uint32_t>(std::strlen(destination));
        std::memcpy(bytes.data()+0x18, &text, sizeof(text));
        std::memcpy(bytes.data()+0x20, &length, sizeof(length));
        std::memcpy(bytes.data()+0x58, &cargo, sizeof(cargo));
    }
    std::uintptr_t Address() { return reinterpret_cast<std::uintptr_t>(bytes.data()); }
    void Expires(std::uint32_t minute) { std::memcpy(bytes.data() + 0x48, &minute, sizeof(minute)); }
};

int main() {
    Check(ManualAttemptBudget(0,10)==5 && ManualAttemptBudget(1,10)==4 && ManualAttemptBudget(4,10)==1);
    Check(ManualAttemptBudget(5,10)==0 && ManualAttemptBudget(0,2)==2 && ManualAttemptBudget(0,0)==0);
    Check(ManualAttemptBudget(0,10,true)==6 && ManualAttemptBudget(4,10,true)==2);
    bool manual = false; std::uint32_t active_budget = 1;
    { ManualGenerationScope scope(manual,active_budget,5); Check(manual && active_budget==5); }
    Check(!manual && active_budget==1);
    for (unsigned slot : {0x70u,0x78u}) {
        std::uint32_t budget = 1;
        const auto code = MakeAttemptBudgetStub(reinterpret_cast<std::uintptr_t>(&budget), slot);
        auto* executable = VirtualAlloc(nullptr,4096,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE); Check(executable);
        std::memcpy(executable,code.data(),code.size()); DWORD previous = 0;
        std::memcpy(static_cast<unsigned char*>(executable)+64,AttemptBudgetUnwindInfo.data(),AttemptBudgetUnwindInfo.size());
        Check(VirtualProtect(executable,4096,PAGE_EXECUTE_READ,&previous));
        FlushInstructionCache(GetCurrentProcess(),executable,code.size());
        RUNTIME_FUNCTION function{0,static_cast<DWORD>(code.size()),64};
        Check(RtlAddFunctionTable(&function,1,reinterpret_cast<DWORD64>(executable)));
        DWORD64 lookup_base = 0;
        Check(RtlLookupFunctionEntry(reinterpret_cast<DWORD64>(executable)+13,&lookup_base,nullptr)==&function);
        for (unsigned offset : {0u,1u,13u,22u}) {
            std::array<DWORD64,3> stack{0x1122334455667788,0x123456789abcdef0,0};
            CONTEXT context{};
            context.Rip = reinterpret_cast<DWORD64>(executable)+offset;
            context.Rsp = reinterpret_cast<DWORD64>(stack.data()+(offset==0 || offset==22 ? 1 : 0));
            PVOID handler = nullptr; DWORD64 frame = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER,reinterpret_cast<DWORD64>(executable),context.Rip,&function,&context,&handler,&frame,nullptr);
            Check(context.Rip==stack[1] && context.Rsp==reinterpret_cast<DWORD64>(stack.data()+2));
        }
        for (unsigned value : {1u,2u,5u,10u,0xfedcba98u}) {
            budget = value;
            Check(ProbeBudgetStub(executable,slot,0)==value);
            Check(ProbeBudgetStub(executable,slot,1)==value);
        }
        Check(RtlDeleteFunctionTable(&function));
        VirtualFree(executable,0,MEM_RELEASE);
    }

    constexpr std::uint32_t deadline = 10000;
    for (unsigned i = 0; i < 10; ++i) {
        const auto seed = ManualTimerSeed(deadline,12345,i);
        Check(seed>deadline && seed<=deadline+1000000);
        for (unsigned j = 0; j < i; ++j) Check(seed!=ManualTimerSeed(deadline,12345,j));
    }

    Offer old("destination.a",1), duplicate_old("destination.a",1),
          different_cargo("destination.a",2), different_destination("destination.b",1),
          duplicate_new("destination.b",1);
    std::array<std::uintptr_t,8> pointers{old.Address(),duplicate_old.Address(),different_cargo.Address(),
        different_destination.Address(),duplicate_new.Address()};
    GameOfferVector offers{123,pointers.data(),5,pointers.size()};
    std::vector<std::uintptr_t> released;
    auto release = [&](GameOfferVector& removed) { Check(removed.vtable==123); released.insert(released.end(),removed.data,removed.data+removed.count); };
    Check(PruneNewOffers(offers,1,release).duplicates==2);
    Check(offers.count==3 && pointers[0]==old.Address() && pointers[1]==different_cargo.Address() && pointers[2]==different_destination.Address());
    Check(pointers[3]==0 && pointers[4]==0 && released.size()==2);
    Check(released[0]==duplicate_old.Address() && released[1]==duplicate_new.Address());
    Check(PruneNewOffers(offers,3,release).duplicates==0 && released.size()==2);

    // Reproduce the first-slot trap: the special path yields the same logical offer;
    // the ordinary paths in slots 1..4 yield distinct destinations. Count unique additions only.
    Offer special("special",1), repeated_special("special",1),
          a("a",1), b("b",1), c("c",1), d("d",1), e("e",1);
    pointers = {}; offers = {123,pointers.data(),0,pointers.size()};
    auto invoke = [&](unsigned budget) {
        std::array<std::uintptr_t,6> candidates{offers.count ? repeated_special.Address() : special.Address(),a.Address(),b.Address(),c.Address(),d.Address(),e.Address()};
        for (unsigned slot = 0; slot < budget; ++slot) offers.data[offers.count++]=candidates[slot];
    };
    invoke(1); invoke(1);
    Check(PruneNewOffers(offers,0,release).duplicates==1 && offers.count==1);
    pointers = {}; offers.count=0;
    invoke(ManualAttemptBudget(0,10,true));
    const auto capped = PruneNewOffers(offers,0,release);
    Check(capped.duplicates==0 && capped.excess==1 && offers.count==5);
    // An existing special offer must not consume any of the five NEW offers.
    pointers = {}; pointers[0]=special.Address(); offers.count=1;
    invoke(ManualAttemptBudget(0,10,true));
    Check(PruneNewOffers(offers,1,release).duplicates==1 && offers.count==6);
    // With four new offers already retained, the retry must reach regular slot one.
    pointers = {special.Address(),a.Address(),b.Address(),c.Address(),d.Address()}; offers.count=5;
    const auto last_budget = ManualAttemptBudget(4,10,true);
    std::array<std::uintptr_t,2> last{repeated_special.Address(),e.Address()};
    for (unsigned slot=0;slot<last_budget;++slot) offers.data[offers.count++]=last[slot];
    Check(PruneNewOffers(offers,1,release).duplicates==1 && offers.count==6);
    // If there is only one valid combination, repeats cannot manufacture five unique offers.
    pointers = {}; offers.count=0;
    for (unsigned attempt=0;attempt<20 && offers.count<5;++attempt) {
        const auto budget=ManualAttemptBudget(offers.count,10,true);
        for (unsigned slot=0;slot<budget;++slot) offers.data[offers.count++]=special.Address();
        PruneNewOffers(offers,0,release);
    }
    Check(offers.count==1);
    GameOfferVector empty{}; Check(PruneNewOffers(empty,0,release).duplicates==0);

    // Automatic stock, two manual replacements, then automatic stock at the latest deadline.
    auto release_batch = [&](GameOfferVector& batch) {
        release(batch);
        std::fill_n(batch.data, batch.count, 0);
        batch.count = 0;
    };
    pointers = {};
    offers.count = 0;
    unsigned automatic_calls = 0;
    auto automatic = [&] {
        ++automatic_calls;
        offers.data[offers.count++] = special.Address();
    };
    special.Expires(100);
    RefillEmptyOwnedOffers(offers, 1, automatic, release_batch);
    Check(offers.count == 1 && automatic_calls == 1);

    std::array<Offer*, 6> first{&a, &b, &c, &d, &e, &repeated_special};
    std::array<Offer, 5> second{{{"f",1}, {"g",1}, {"h",1}, {"i",1}, {"j",1}}};
    constexpr std::uint32_t first_deadline = 100 + 48 * 60;
    constexpr std::uint32_t second_deadline = 110 + 48 * 60;
    const auto first_refresh = ReplaceOwnedOffers(offers, 10, true, [&](std::uint32_t budget, unsigned attempt) {
        Check(attempt == 0 && budget == 6 && offers.count == 0);
        for (unsigned i = 0; i < budget; ++i) {
            first[i]->Expires(first_deadline);
            offers.data[offers.count++] = first[i]->Address();
        }
    }, release_batch);
    Check(offers.count == 5 && first_refresh.excess == 1);
    RefillEmptyOwnedOffers(offers, 105, automatic, release_batch);
    Check(offers.count == 5 && automatic_calls == 1);

    const auto released_before = released.size();
    ReplaceOwnedOffers(offers, 10, false, [&](std::uint32_t budget, unsigned attempt) {
        Check(attempt == 0 && budget == 5 && offers.count == 0);
        for (unsigned i = 0; i < budget; ++i) {
            second[i].Expires(second_deadline);
            offers.data[offers.count++] = second[i].Address();
        }
    }, release_batch);
    Check(offers.count == 5 && released.size() == released_before + 5);
    Check(offers.data[0] == second[0].Address());
    RefillEmptyOwnedOffers(offers, first_deadline, automatic, release_batch);
    Check(offers.count == 5 && automatic_calls == 1);
    special.Expires(second_deadline + 48 * 60);
    RefillEmptyOwnedOffers(offers, second_deadline, automatic, release_batch);
    RefillEmptyOwnedOffers(offers, second_deadline + 1, automatic, release_batch);
    Check(offers.count == 1 && automatic_calls == 2 && offers.data[0] == special.Address());

    // Empty or ineligible companies get a single attempt on each native refresh.
    unsigned failed_attempts = 0;
    RefillEmptyOwnedOffers(empty, second_deadline, [&] { ++failed_attempts; }, release_batch);
    Check(failed_attempts == 1 && empty.count == 0);
    std::puts("Budget stub execution/unwinding, manual scope, slot variety and native-key duplicate filtering passed.");
    std::puts("Owned cargo follows 1 -> 5 -> 5 -> 1, with one automatic attempt after expiry.");
}
