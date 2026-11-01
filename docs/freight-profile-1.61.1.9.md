# Freight profile, ETS2 1.61.1.9 — revision after 0.4 crash

Executable SHA-256: B70C2357CC56EB5834EE47FEA3C60B2F2D4591383FF2AC479CFD3CD254301B8C.

## Confirmed failure in 0.4

The reported log records F9 completion (29783 offers counted, 5647/6573 companies with five), followed by an assertion in array_t<node_item_t*>: index 2 >= size 2. The latest crash is dated 2026-11-01, build 1.61.1.9, exception C0000025.

Crash RVAs derived from the reported EXE base: 0x6fd505 (accessor function starts 0x6fd4b0), 0x6fe28e (physical parking updater starts 0x6fe1c0), 0x6fb5f8 and 0x6fa796.

The node accessor reads the physical node array at permanent company data +0x70, count +0x78. The physical update loop uses parking handles at +0xd8/count +0xe0 and passes each index to the physical node accessor. Extending parking handles and configured slot counts without real map nodes violates this invariant. Cloning slot flags does not add physical map nodes.

The 0.4 slot-expansion design is rejected. Do not restore EnsureDescriptors or its parking/configured-count writes as a crash workaround. Version 0.4 binaries are retained only for diagnosis and must not be used.

## 0.4.1 replacement mode

The user approved replacing F9 offers. For each valid company the implementation now calls the same native sequence seen in economy_refresh (RVA 0x4a0060):

1. company_jobs_clear(company, economy), RVA 0x7d0ce0.
2. company_jobs_fill(company, economy), RVA 0x7cd0f0.

Clear releases old base-market offers and removes old company refresh events. Fill creates the native/map-defined number of slots. The DLL no longer allocates offer objects directly, appends slots, expands descriptor arrays, clones flags, changes configured counts, or modifies parking arrays. The generator and company-clear entry hooks have been removed.

One schedule hook remains at RVA 0x81cec9, expected bytes 48 89 5c 24 10. The prologue trampoline executes this complete stack-store instruction and jumps back to +5, preserving the entry null-company guard. During manual refresh only, successful regular offers receive a 48-game-hour scheduled expiry. Native retries, special events and all automatic calls keep their stock timing. Valid resulting offer timestamp fields +0x48 and +0x60 are normalized to that deadline.

The base-offer vector is at company+0x78. The owned-trailer vector at +0xa0 and its separate limiter are not cleared by this F9 path. F8 retains three independent 48-game-hour slots. F9 uses a monotonic five-real-second cooldown after each completed refresh, including empty results, with no run quota. Session reset clears the cooldown.

## Verification limits

Compilation and offline timer checks do not prove physical vehicle handling, menu eligibility, offer acceptance or save/reload behavior. Test 0.4.1 with a copy of a save from before 0.4 F9. The new build does not repair a save containing incompatible expanded slots from 0.4.
