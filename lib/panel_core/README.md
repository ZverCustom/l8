# panel_core — L8 panel mode core (task 027, v6 §4.2, §5–§6, §12, §14.3–14.4, §15)

One frame stream in, one parser out (§15 "один разборщик"). Portable C++17, sans-IO, no
heap; time is the caller's `u32` ms. Not in the `l8_v3` image yet (nothing in `src/`
includes it). Written and committed **before** the code.

## Shape

`PanelCore` is built over caller buffers (receive assembly, response, tid ring) and a
`CoreIo` of plain function pointers:
- `tx(frame, singleShot)` — a frame to put on the bus now;
- `dispatch(req, len, resp, cap)` — the device's command layer; returns the response
  length (0: no response);
- `statusA(flags, rev, saved, chOn, chFault)` — STATUS_A contents from the device;
- `log(text)` — counters and events for tests.

Input: `onFrame(frame, panel_rx::Stamp)` for every frame from the queue (§14.3 stamp from
`panel_rx::QueueStamp`), `tick(now)`, `singleShotResult(sent|lost|busError)`.

Routing, one parser:
| Frame | Goes to |
|---|---|
| VEH_FAST / VEH_SLOW / PANEL_HB / PANEL_STATE (exact IDs) | `panel_rx::Consumer`; PANEL_HB also to discovery (panelGen, §4.2) |
| DISCOVER, ASSIGN, BEACON/ANNOUNCE of others | `zmb::ModuleDiscovery` (§4.2) |
| XSTART / XDATA / XABORT with `dst` = own address, `src` 0x00 | transport receiver |
| FLOW / ACK / RABORT with `dst` = own address, `src` 0x00 | transport sender |
| anything else (standard ID, RTR, DLC ≠ 8, other `dst`, other `src`, other functions) | dropped and counted by reason |

Transmit only per §4.2/§15: before an ASSIGN of this boot only ANNOUNCE and BEACON
(single-shot); after it HELLO, transport frames and STATUS_A. No OBD, no bitrate search.

## Device side (host build)

`tools/panel_node.cpp` = `PanelCore` + the **real** `l8proto` (`src/protocol`,
`src/settings`) built for the host with the existing test stubs (`test/support`, the same
fakes as `test_protocol`). A REQUEST assembled by the transport goes to
`l8proto::handleV1(data, len, Transport::Usb, true, http)` — the code HTTP and USB use —
and its reply goes back as RESPONSE. `panel_node` speaks the `zmb_node` line protocol, so
the D10 rig runs either; the scripted `zmb_node` stays, unchanged, as a separate binary
whose answers say `"model":"scripted"`. `panel_node` logs `E exec <cmd> <id> <revision>
<stateHash>` **after** the dispatcher returned (the journal is the dispatcher's), and has
`DIRECT <json>`: the same dispatcher without the bus (parity reference).

Bus access = USB access: the bus has no token, so the HTTP-only `AuthRequired` check does
not apply; every other check of the common contract (bootId, revision, editor lease,
preview expiry, `OUTPUT_BLOCKED`, master-off for reboot, OTA busy, sizes) applies
unchanged. Nothing is added beyond `l8proto`.

## Rules of the core

- No dispatch before HELLO_ACK of the current address/generation.
- A REQUEST is dispatched only from a complete, CRC- and header-checked message
  (`zmb::Receiver`), never from a partial one.
- Not executed twice: the receiver ignores a duplicate XSTART within 1 s; on top, the core
  remembers the last executed (panelGen, tid, crc) and does not dispatch it again for 60 s
  (tid quarantine), counted `dup_request`.
- One response at a time: a REQUEST arriving while the previous RESPONSE is still being
  sent is **not dispatched** (`busy_dropped`) — never executed without a way to answer.
- Boundaries (§4.2): a new panelGen, RELEASE, a duplicate address in a BEACON, HELLO_ACK
  timeout → discovery generation changes → the core drops the assembly, the sender, the
  pending response and the tid lease; a new boot of the module is a new `PanelCore` with a
  new bootId.
- `vehicle` source for panel mode: `panelVehicle(now)` fills the fields of the existing
  `VehicleData` (rpm, speed km/h, throttle, temp °C, `lastUpdate` = `m` of rpm, never 0)
  so that the existing rules — `vehicleDataFresh()` = `lastUpdate && now − lastUpdate ≤
  3000`, `engineRunning()` = fresh && rpm > 300 (`vehicle.cpp:200-201, 242`) — give the
  §14.4 answer. Not wired into the firmware here.

## Acceptance criteria

| ID | Expected |
|---|---|
| C1 | routing: every frame kind reaches only its consumer; others dropped by reason (counters) |
| C2 | before ASSIGN only ANNOUNCE/BEACON are transmitted; after ASSIGN HELLO, then transport and STATUS_A |
| C3 | transport: own address only; frames with another `dst` or `src` ≠ 0x00 change nothing |
| C4 | a REQUEST reaches the dispatcher once, complete; its reply is the RESPONSE (reqTid/reqCrc) |
| C5 | duplicate XSTART / repeated request (same tid+crc) / busy → not dispatched again |
| C6 | boundaries: new panelGen, RELEASE, duplicate address, HELLO_ACK timeout, reboot — assemblies and sends end, nothing executed from a partial message, state after |
| C7 | `panelVehicle`: running, ECU freeze, dashcore freeze, link loss → `engineRunning`/`vehicleDataFresh` by §14.4 and by the existing `vehicle.cpp` rule (3 s) |
| C8 | bad input fuzz: no crash, no dispatch from an invalid frame sequence |
| C9 | no heap in the core, sizes named; native tests -Werror + ASan/UBSan |
| C10 | end to end: HTTP → dispatcher → owner → `LinkLower` → `McuCore` → model bus → `panel_node`; `info`, `status`, `get_config`, `preview`, `apply`, `save` answer exactly what the same dispatcher answers via `DIRECT` (a second process, same seed, same order, same time) |
| C11 | the 016 scenarios and the 022–024 faults on `panel_node`: each marked moved / not applicable / stays on the scripted node; the 022 reconciliation rule with the dispatcher's journal (`responded` ⇒ executed once, state changed once; `aborted`/`queued` ⇒ state unchanged) |
| C12 | previous L8 suites and `l8_v3` unchanged (sections, symbols); D10 previous checks on the final commits |
| C13 | ≥ 3 mutants (routing, address check, repeated execution) caught |

## Task 028 (R71): freshness per field in the vehicle view — before the fix

### How the existing code reads the fields (`src/`, unchanged)

| Field | Readers | Freshness they use | "No data" today |
|---|---|---|---|
| rpm | `engineRunning()` (`vehicle.cpp:242`) → `power.cpp:35` (`engineCut`), `led_control.cpp:390` (`engineOff`); shift light `led_control.cpp:243`; rpm scale `led_control.cpp:235` | the one stamp `lastUpdate`, 3 000 ms (`vehicleDataFresh`) | `vehicle.cpp:173-177`: after 3 s the value is set to 0 |
| speed | speed scale `led_control.cpp:228`; `hub.cpp:93` (status JSON) | **the rpm stamp** (`scaleFraction`: `source<=2 && !vehicleDataFresh()` → 0) | 0 (same reset) |
| throttle | throttle scale `led_control.cpp:229`; `hub.cpp:93` | **the rpm stamp** | 0 (same reset) |
| temp | `hub.cpp:93` only | none | none: the value stays (not reset) |

So in the existing code one stamp serves all fields — fine for one CAN source that decodes
them together, wrong for the panel, where §14.4 gives each field its own `m` and visible
freshness of 300 ms. R71: `panelVehicle` filled speed/throttle/temp whenever the field
had ever been received, under the rpm stamp.

### Choice

The view carries freshness per field **and** puts the existing "no data" value into a
field that is not fresh:
- `speed`, `throttle`: not fresh (`now − m > 300`) → 0, the existing "no data" (what the
  stale reset and `scaleFraction` already produce). Existing readers stay correct unmodified.
- `temp`: the existing code has no "no data" value (0 °C is a real temperature) → 0 and
  `fresh` bit cleared; a reader must use the bit (stated for the firmware wiring).
- `rpm`: the value and `lastUpdate` stay as they are (3 s rule): zeroing rpm at 300 ms
  would change `engineRunning()` and the 3 s threshold, which must not change. The view
  adds `rpmFresh` (300 ms) for scales/shift light; using it there is part of the later
  firmware wiring.
- `fresh` mask bits: 0 rpm, 1 speed, 2 throttle, 3 temp (coolant), each `now − m ≤ 300`
  of that field in `panel_rx`.

### Criteria

| ID | Expected |
|---|---|
| V1 | the reviewer's scenario: one frame with all fields, then 5 s of frames with only rpm valid → speed 0, throttle 0, temp 0, their fresh bits clear; rpm fresh, `lastUpdate` young |
| V2 | boundaries 300 / 301 ms for each of the seven fields (`panel_rx::fresh` for all; the view's bit and value for rpm, speed, throttle, temp) |
| V3 | the reverse direction kept: a fresh speed does not move rpm's `lastUpdate` / `engineRunning`; `engineRunning` and the 3 s threshold unchanged (C7 still passes) |
| V4 | mutant "the view ignores the field's freshness" caught |
| V5 | the other outputs used by the firmware checked for one stamp serving several fields (result in the report) |
