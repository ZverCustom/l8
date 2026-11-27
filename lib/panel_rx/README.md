# panel_rx — L8 receive side of the panel frames (task 026, v6 §7, §14.3, §14.4, §15)

Portable C++ library for the L8 in panel mode: the software filter and parser of
VEH_FAST / VEH_SLOW / PANEL_HB / PANEL_STATE, the queue time stamp of §14.3 ("software
queue → task"), VEH `seq`, the per-field `m` and the consumer policy of §14.4. No TWAI, no
firmware wiring: the `l8_v3` image does not contain it (nothing in `src/` includes it).

Written and committed **before** the implementation and its tests.

## Decisions

- **Filter (§15).** Accepted only: extended ID, DLC 8, and the exact IDs of §2/§7 —
  VEH_FAST `0x04103F00`, VEH_SLOW `0x04203F00`, PANEL_HB `0x08403F00`, PANEL_STATE
  `0x08503F00` (prio, func, `src 00`, `dst 3F`, `tid8 0`). A frame with one of these
  functions but another prio / src / dst / tid / DLC, or a standard ID, is dropped and
  counted by reason. Other functions are "not ours" (left to `lib/zmb`), counted apart.
- **Queue stamp (§14.3) as a token API.** `Poll p = stamp.poll(t0, queueEmpty, overrunGrew)`
  is the step before the driver read: `t0` is taken by the caller *before* it looks at
  `msgs_to_rx`; an empty queue sets the anchor to `t0`; grown overrun counters clear it.
  `Stamp s = stamp.read(p, tRead, overrunGrew)` gives the frame read with that token its
  anchor: the one of `p`, never a later one. Usable only if an anchor exists and
  `tRead − anchor ≤ 50 ms`; overrun at read clears both. A frame can only be read through
  a `Poll`, so "stamp before the emptiness check" and "a frame uses the stamp taken before
  its read" follow from the interface.
- **Field time.** For a usable frame, every field with its `valid` bit gets
  `m = anchor − age × 4 − 10` ms (10 = 5.2 bus + `H_max` 2 + margin; `H_max` named
  constant, an assumption checked only on the bench). Fields without `valid` keep their
  `m`. `age = 0xFF` means no valid field; `age = 0xFE` (≥ 1 016 ms, no upper bound) with
  valid bits: nothing is updated (an unbounded age could make `engineRunning` true for a
  stale rpm), counted. Reserved `valid` bits are ignored.
- **VEH `seq` (§14.3).** Per type, u8: accepted if `(seq − last) mod 256` ∈ 1…127, else the
  frame is dropped whole. `last` is forgotten (the next frame is accepted) when `mcuGen`
  (PANEL_STATE) or `panelGen` (PANEL_HB) changes, or after 1 s without a frame of that type.
- **Consumer (§14.4).** Per field: value and `m`. Visible freshness: `now − m ≤ 300`.
  `engineRunning = now − m_rpm ≤ 3 000 && rpm > 300`; `vehicleDataFresh` = rpm fresh by
  3 000 ms; other fields never extend rpm. Thresholds of `settings.h` / `power.cpp` are
  not touched; the library only gives the values.
- **PANEL_HB / PANEL_STATE.** `panelId`, `panelGen`, `mcuGen`, `gatewayAlive`,
  `vehLinkOk`, `vehAgeMax` are exposed as received (with their L8 receive time). The norm
  gives `gatewayAlive = 0` no output behaviour; the library attaches none (proposal in the
  report: informational only; IGN, Master, `inhibit`, current protection stay local, §15).
- **Time and memory.** Only `u32` ms passed in, modular (§1); no heap, no exceptions;
  sizes named by the tests.

## Acceptance criteria

| ID | Expected |
|---|---|
| L1 | §7 example and temperatures byte for byte; every frame of D10's `mcu/link/vectors/veh.txt` (task 021) parses to the generator's fields |
| L2 | filter: standard ID, DLC ≠ 8, wrong prio / src / dst / tid of a panel function → dropped by reason; foreign functions counted as not ours |
| L3 | stamp: the §14.5 L8 rows — norm (estimated age ≥ true), a frame 500 ms in the L8 queue (not usable), the tail of an old burst after a 2 s task pause (none usable; the first new frame uses the new anchor), the emptiness-snapshot race (anchor = `t0` taken before the check → not usable), FIFO overrun (no anchor until the next empty snapshot) |
| L4 | fields: mixed `valid` (speed fresh does not extend rpm), ECU freeze (rpm without valid → `engineRunning` false after 3 s), dashcore freeze, MCU restart (new `mcuGen` → seq reset, first frame accepted, no valid until the MCU allows) |
| L5 | seq: 254, 255, 0, 1 accepted, then 253 dropped; +127 accepted, +128 dropped; reset by `mcuGen`, `panelGen`, 1 s silence; FAST and SLOW apart |
| L6 | freshness property: model with known measurement times, MCU age (conservative), bus ≤ 5.2 ms, `H` ≤ `H_max`, random queue waits and task stalls: for every field `m` ≤ true measurement time (age never under-estimated), and no field is updated from a frame read > 50 ms after its anchor |
| L7 | end to end with the accepted MCU (`link_node` with `McuCore`): VEH frames from `Panel` reach a `panel_rx` process; rpm age on the L8 side never below the true age on the whole path source → CM5 → MCU → L8 |
| L8 | bad input: random IDs, DLC, seq backwards, foreign generations, bursts — no crash, no `valid` from an invalid frame |
| L9 | native tests with -Werror and ASan/UBSan; previous L8 suites (`native`, `native_settings`, `native_zmb_san`) unchanged; `l8_v3` builds and does not contain the library (symbols, size) |
