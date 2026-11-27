# zmb — ZMB v1 transport core (C++)

The module side of the D10 module bus, contract
`bobr-projects/ZVER/app/specs/2026-11-27-unified-configurator-v6.md` §1–§2, §5–§6, §12.
The independent Rust core lives in d7-d10 (`src/zmb`). Both are checked against the same
corpus `test/test_zmb/corpus_v1.txt` and against each other (d7-d10
`scripts/zmb-interop.sh`).

**Not used by the firmware yet:** no TWAI/CAN, LED or power path includes it. The
`l8_v3` image contains no `zmb::` symbols.

## API

- **Wire:**
  - `AddrId` (29-bit ID), `build`/`parse` of XSTART/XDATA/XABORT/FLOW/ACK/RABORT;
  - `Header` (20 bytes), `crc32` (IEEE), `Hello`/`HelloAck` (§6.2/§6.3);
  - big-endian; reserved bytes written 0 and ignored;
  - `parse` rejects standard (11-bit), RTR, DLC ≠ 8 and out-of-range frames.
- **`Sender` (§5.3):**
  - `start(msg, len, tid)`, then `poll(now, frame)` → offered candidate (the same until
    `confirm(now)` or `refuse()`);
  - `onFrame`, `tick`, `cancel`, `generationChanged`;
  - **committed** only when the lower layer confirms the write of the last XDATA. Before
    that, `cancel` is `NotDelivered`; after it, `Unknown`;
  - no XDATA without a matching FLOW CTS, never more than its block, at least `stmin`
    apart;
  - a WAIT more than 3 times in a row, or N_Bs (150 ms) → XABORT TIMEOUT. T_ack
    (150 ms) → `Unknown`;
  - a late RABORT after the last XDATA proves nothing.
- **`Receiver` (§5.4):**
  - one assembly per sender into a **caller buffer** (`buf`, `cap ≤ 24 596`);
  - XSTART supersede, seq/tid8/src/dst checks, the previous seq repeated is ignored, a
    gap → RABORT SEQ_GAP, N_Cr (100 ms) → dropped silently;
  - on completion, in this order: CRC, header (ver, tid), `srcAssigned`, type for the
    side, generations (`panelGen`, own bootTag; HELLO: bootTag = bootId[0..4] and MAC of
    the assigned src in deviceId). Only then the `Message` event;
  - the ACK comes in the same output and **must be queued before the handler runs**;
  - **credit:** a FLOW CTS is only a candidate when formed: `reply` + a local
    `FlowTicket` in `out.flow` (also `pendingFlow(frame, ticket)`). CTS frames of two
    assemblies can be byte-identical (same tid, another CRC or panelGen); their tickets
    never are. The block becomes acceptable only after `confirmFlow(ticket, now)` of the
    **current** candidate. A held, refused, repeated or stale ticket (superseded, timed out,
    another generation) grants nothing and changes nothing — not the current candidate,
    not its timer. XDATA beyond the granted block → RABORT OVERFLOW, no message. While our
    CTS waits, the assembly lives N_Bs (the sender gives up then); after it, N_Cr;
  - a delayed copy of the XSTART just delivered (same context and generations, within
    `kDupWindowMs` = 1 s) is ignored: no ACK, no FLOW, no second delivery. A new
    generation, another context, or after 1 s (tid reuse is legal after 60 s) → a fresh
    assembly;
  - unknown FLOW/ACK/ABORT codes are dropped by `parse` (§1); an XSTART/XABORT/XDATA
    with another tid8 changes nothing. Both count in `invalid` (Sender and Receiver).
    A valid ACK ≠ 0 is still `Rejected`.
- **`matchReply`** — reply matching without a JSON id (HELLO_ACK); RESPONSE/JSON-id
  matching is the panel's (Rust).

## Time and memory

- Time is `uint32` ms given by the caller; wrap-around is handled by `expired()`.
- No allocation, no sleeps:
  - `Sender` reads the caller's message in place (keep it alive until the outcome);
  - `Receiver` writes only into the caller's buffer.
- Sizes (host, 64-bit): `Sender` 56 bytes, `Receiver` 112 bytes + the caller buffer.

## What a future adapter must confirm

`confirm()` / `confirmFlow()` only after the frame is really in the transmit path (TWAI
queue / CAN_TX).

FLOW order for the adapter: keep the ticket with the queued CTS; right before writing it,
`isCurrent(ticket)` — if false, drop it unsent (even when its bytes equal the current
candidate); after the write is accepted, `confirmFlow(ticket, now)`. A late callback with
an old ticket returns false and leaves the new assembly untouched. Tickets are a wrapping
`uint32` counter per receiver (0 never issued).
The last XDATA's confirm is the irreversibility boundary of §12. A refused or dropped
frame → `refuse()`, never `confirm()`.

## Clarifications taken (v6 not changed)

- XSTART with `len < 20` or larger than the caller buffer → FLOW OVERFLOW;
  `len > 24 596` → RABORT OVERFLOW.
- FLOW CTS with another `expected` or `block = 0` is ignored; N_Bs decides.
- A message from a `src` not assigned in the current panelGen → ACK GEN_MISMATCH.

## Transaction ids (`TidTable`, §5.5)

The module's u16 counter for its messages to the panel, one per panelGen (`reset` on a new
one).
- **Ring in allocation order over a caller buffer** of `TidEntry` (8 bytes each; future
  PSRAM). Entry i is the allocation `nextSerial − len + i`, tid = serial & 0xFFFF.
- The oldest entry leaves only when `finish`ed and its last frame is ≥ 60 s old; `touch`
  moves "last" on every frame. The counter only moves forward.
- **Full ring** → `alloc` returns false: the module waits (`freeAt`), never reuses a tid
  early. An active entry never ages out.
- `alloc` returns a `TidLease` (table instance, reset generation, serial, tid).
  `touch`/`finish` take the lease. A lease of another table instance (even over the same
  buffer), of a counter space before `reset`, or of an entry that already left changes
  nothing. Wrap of u16 and of the u32 clock is handled.
- No allocation, no clock of its own. A module sends one message at a time, so a small
  buffer is enough (the peer uses 256).

## Discovery, module side (`zmb_disc.h`, §4.2)

`ModuleDiscovery` implements rules 1–7 of §4.2, sans-IO.

- **Rule 1**: no addressed frames until an ASSIGN with our MAC and the low byte of the
  last PANEL_HB's panelGen.
- **Rule 2**: wantAddr is the stored address when the stored panelId matches PANEL_HB's.
- **ANNOUNCE timing** (single-shot):

  | When | Delay |
  |---|---|
  | first attempt | 0..200 ms |
  | lost arbitration | 1..20 ms |
  | bus error | 50..250 ms |
  | sent, no ASSIGN | 1000 + 0..250 ms |

  5 bus errors in a row count `announceErrors`.
- **BEACON**: every 1000 ± 125 ms.
- **Our address in another MAC's BEACON**: back to ANNOUNCE with wantAddr 0.
- **RELEASE**: forget the address.
- **New panelGen**: forget the confirmation, keep the address as wantAddr.
- **`generation()`** grows on every loss of the address; the owner then drops its
  transport state.
- **Randomness** comes from a caller function, so tests are deterministic.

The native tests cover trace A of §4.4 and rules 2/5/6/7.

## Gateway test participant (`tools/zmb_node.cpp`)

The module process for the D10 gateway integration tests (task 016), built by
`tools/build_zmb_node.sh` with ASan/UBSan. It is used by `scripts/zmb-gateway.sh` of the
d7-d10 repository.

- It uses `ModuleDiscovery`, `Sender`, `Receiver` and `TidTable` (lease API, reset per
  panelGen).
- It handles HELLO/HELLO_ACK (`matchReply`) and REQUEST → RESPONSE (reqTid, reqCrc).
- It sends STATUS_A every 500 ms and BEACON.
- It reads line commands on stdin: `T`, `F`, `R` for the single-shot result, `REBOOT`,
  `MUTE`, `SILENT`.
- Its business answers (info, status, preview, save, get_config, apply) are scripted.
  They prove the route, not the firmware dispatcher, flash writes or power outputs.

## Not here (next U3b steps)

- the TWAI adapter and panel mode (U3b-l8), H_max;
- the MCU link (U3b-mcu).

## Tests

```
pio test -e native -f test_zmb          # corpus + traces
pio test -e native_zmb_san              # the same with ASan + UBSan
tools/build_zmb_peer.sh                 # interop peer (ASan/UBSan) for d7-d10 scripts/zmb-interop.sh
                                        # (its RESP command answers with a TidTable tid)
tools/build_zmb_node.sh                 # gateway test module process (d7-d10 scripts/zmb-gateway.sh)
```
