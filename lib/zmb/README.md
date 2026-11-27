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
  - a repeated XSTART of the message just delivered is answered ACK OK again, not
    delivered twice.
- **`matchReply`** — reply matching without a JSON id (HELLO_ACK); RESPONSE/JSON-id
  matching is the panel's (Rust).

## Time and memory

- Time is `uint32` ms given by the caller; wrap-around is handled by `expired()`.
- No allocation, no sleeps:
  - `Sender` reads the caller's message in place (keep it alive until the outcome);
  - `Receiver` writes only into the caller's buffer.
- Sizes (host, 64-bit): `Sender` 56 bytes, `Receiver` 56 bytes + the caller buffer.

## What a future adapter must confirm

`confirm()` only after the frame is really in the transmit path (TWAI queue / CAN_TX).
The last XDATA's confirm is the irreversibility boundary of §12. A refused or dropped
frame → `refuse()`, never `confirm()`.

## Clarifications taken (v6 not changed)

- XSTART with `len < 20` or larger than the caller buffer → FLOW OVERFLOW;
  `len > 24 596` → RABORT OVERFLOW.
- FLOW CTS with another `expected` or `block = 0` is ignored; N_Bs decides.
- A message from a `src` not assigned in the current panelGen → ACK GEN_MISMATCH.

## Not here (next U3b steps)

- tid manager with the 60 s quarantine;
- multi-module dispatch;
- bus budget/prepayment, priorities, discovery;
- the TWAI adapter and panel mode (U3b-l8), H_max;
- the MCU link (U3b-mcu).

## Tests

```
pio test -e native -f test_zmb          # corpus + traces
pio test -e native_zmb_san              # the same with ASan + UBSan
tools/build_zmb_peer.sh                 # interop peer (ASan/UBSan) for d7-d10 scripts/zmb-interop.sh
```
