# Base-company offer profile, ETS2 1.61.1.9

This is a record of local binary analysis for the experimental F9 implementation,
not a stable SCS interface. No CargoMarketFix source is included.

Executable SHA-256: `B70C2357CC56EB5834EE47FEA3C60B2F2D4591383FF2AC479CFD3CD254301B8C`.
All RVAs are relative to the executable image base.

## Structures and native routines

- Economy pointer: RVA `0x3046780`. Company pointer array at economy `+0x28`,
  count at `+0x30`, economy time in game minutes at `+0x19c`.
- Cargo-market offers: company vector at `+0xa0` (existing owned-trailer mode).
- Base-company offers: separate vector at `+0x78`, data `+0x80`, count `+0x88`.
  New F9 orders are added to this base vector. The two UI sections still require
  in-game verification for visibility, eligibility filters and accepting an order.
- Company permanent/slot data pointer at `+0x10`: slot flags vector at `+0x90`
  with 32-bit elements; configured offer count at `+0xc0`.
  If flag `+0xc4 & 2` is present, parking handles vector starts at `+0xd0`.
- Native base-offer initialization `0x7cd0f0` allocates objects with `0xaba700`,
  appends vector slots using `0xf36c0`, and calls `0x7cded0` per slot.
- `0x7cded0(company, economy, minute, slot_index, forced_regular)` mutates one
  existing slot and reads the matching slot descriptors. Calling it without first
  provisioning a new object/slot would replace an existing offer.
- `0x81cec0(scheduler, economy, expiry_minute, company, slot_index)` schedules a
  future native refresh. During manual candidate attempts these events are
  suppressed; one event is added only after a valid offer is committed.
- Object construction uses the native allocator context at RVA `0x367f918`,
  indexed by the global at `0x2733298`, and the same reference transitions as
  the locally inspected native initialization sequence.

## Hook boundaries

All replaced instructions are complete five-byte stack stores, with no relative
address operands. Each trampoline executes the original store before returning
to the untouched body. Entry bytes are validated before any patch is installed.

| Site RVA | Expected bytes | Purpose |
| --- | --- | --- |
| `0x7cded0` | `4c 89 4c 24 20` | Prepare descriptors before native slot generation |
| `0x81cec9` | `48 89 5c 24 10` | Suppress intermediate manual-attempt scheduling |
| `0x7d0ce0` | `48 89 5c 24 08` | Keep native company clearing within descriptor bounds |

The scheduling hook is nine bytes after the entry, preserving the original
null-company guard and avoiding relocation of its conditional branch.
If any hook fails, all F9 hooks are removed and the mode is disabled.

## Persistence and remaining validation

Additional offers require matching slot descriptors. F9 extends descriptors with
native allocation, clones existing slot flags and initializes parking handles.
Failed additions roll back descriptor counts and release handles using the
native parking-handle helper (`0x1086f0`). New jobs are never inserted by reusing a
live slot, so old refresh events cannot overwrite them.

Descriptors are reconciled after load and before native generation/clearing.
This is structurally derived, not proof that every load/serialization path has
been covered. Save/reload, selecting and completing an F9 order, and expiration
with native automatic refresh must be tested on a disposable profile before
using this feature on a main save. Loading a save with extra slots without the
plugin is not validated. Keep a save from before the first F9 use.

Native automatic generation remains enabled without an attempt-count patch in
this market. Extra slots are subject to native automatic refresh. The manual
mode is bounded at 512 slots per company and 20 attempts per requested offer.
