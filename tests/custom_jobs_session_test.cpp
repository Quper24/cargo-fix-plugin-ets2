#include "custom_jobs.h"
#include <cstdio>
#include <cstdlib>
#define Check(value) do { if (!(value)) { std::fprintf(stderr,"Session check failed: %d\n",__LINE__); std::exit(1); } } while(0)
int main() {
    using namespace custom_jobs;
    Check(ContainsName("СОЛЬ","соль") && ContainsName("Груз: Соль","СО"));
    Check(ContainsName("FedEx","fedex") && ContainsName("Coca-Cola","cola"));
    Check(!ContainsName("Pepsi","FedEx") && ContainsName("Москва",""));
    Initialize(0, 0, nullptr);
    const auto first = Snapshot();
    Check(!first->supported && !first->scanned && first->catalog.companies.empty());
    Observe(100,true); Observe(100,true);
    Check(Snapshot()->session==first->session);
    Observe(100,false);
    Check(Snapshot()->session!=first->session && !Snapshot()->scanned);
    Check(first->session+1==Snapshot()->session); // Held render snapshots stay immutable.
    const auto unloaded = Snapshot();
    Observe(100,false); Check(Snapshot()->session==unloaded->session);
    Observe(200,true); const auto loaded = Snapshot();
    Observe(300,true); Check(Snapshot()->session!=loaded->session);
    Scan(300); Check(!Snapshot()->scanned && !Snapshot()->error.empty()); // Unknown EXE never triggers reads.
    Shutdown(); Check(!Snapshot()->supported && Snapshot()->error.empty());
    std::puts("Catalog lifecycle and case-insensitive Cyrillic/brand-name search passed.");
}
