# L8 panel mode in the firmware (task 036, U3b-l8) — design and criteria

Norm: v6 §4, §7, §14.4, §15; A1–A3; the §6.3 proposal; conditions of reviews U3-025,
U3-026 and `lib/panel_core/README.md` (task 028). Written and committed **before** the code.
Build and host tests only: **no board is flashed.**

## Selection and persistence

- `CAN_MODE_PANEL = 4` joins `CanMode`. It is chosen by the existing setting `can.mode`
  (validated range 0…3 → 0…4), saved and applied by the same path as the other modes
  (`settingsApply` → `vehicleSetMode`). Factory default stays `CAN_MODE_SNIFFER`; a board
  with saved settings 0…3 behaves as before.
- What each mode does with the bus is one pure function, `modePlan(mode)`
  (`src/vehicle/mode_plan.h`): listen-only, bitrate, auto-bitrate scan, OBD polling,
  K-line, panel. `vehicle.cpp` uses it instead of the scattered `mode != OBD2` tests. Rows
  0…3 equal the old behaviour (host test); only row 4 has `panel = true`.
- Entering panel mode: TWAI restarts in **normal** mode at **1 Mbit/s** (§15), auto-bitrate
  and OBD are off, `PanelMode::enter` makes a new `PanelCore` with a new `bootId`. Leaving
  it: `PanelMode::exit` drops the core and clears the vehicle view; the new mode's plan
  restarts TWAI as before.

## TWAI in panel mode

- `CANBus` stays the owner of the driver. Panel mode talks to a thin interface
  `PanelPort` (`src/panel_mode/panel_port.h`): `read(frame)` (non-blocking),
  `send(frame, singleShot)`, `rxQueueEmpty()`, `overrunCount()`, `busOff()`, `recover()`,
  `singleShotDone(&outcome)`. The firmware implementation (`src/can_bus/panel_port_twai.cpp`)
  is the only panel code that touches ESP-IDF; on the host a model implements it.
- Filter: hardware accept-all; the core keeps only extended IDs and its own frames (§15).
- §14.3 queue stamp: each loop step takes `t0`, then `rxQueueEmpty()` (`msgs_to_rx == 0`),
  then reads (`panel_rx::QueueStamp`); an overrun counter that grew voids the anchor.
- Single-shot ANNOUNCE/BEACON: TWAI single-shot; the outcome (sent / lost / bus error)
  goes back to `PanelCore::singleShotResult`.
- Bus-off: the port reports it; panel mode answers a pending single-shot with `BusError`,
  calls `recover()` (TWAI recovery, restart when back) and counts it; frames are not
  sent while the bus is off. The core's own timers end what was in flight (§16).

## Vehicle data in panel mode

`PanelMode` writes `PanelCore::panelVehicle(now)` into the existing `vehicle` (rpm,
speed km/h, throttle, temp °C, `lastUpdate` = `m` of rpm) and a new mask `vehicleFresh`
(bits of `panel_core::kFresh*`). Every reader, and the decision for it:

| Place | Field | Other modes (unchanged) | Panel mode |
|---|---|---|---|
| `led_control.cpp` `scaleFraction` source 0 (rpm scale) | rpm | `vehicleDataFresh()` (3 s) | `kFreshRpm` (300 ms) |
| `scaleFraction` source 1 (speed scale) | speed | `vehicleDataFresh()` | `kFreshSpeed` (value is also 0 when stale) |
| `scaleFraction` source 2 (throttle scale) | throttle | `vehicleDataFresh()` | `kFreshThrottle` |
| `led_control.cpp` `shiftLightActive` | rpm | `vehicleDataFresh()` | `kFreshRpm` |
| `led_control.cpp` engine-off dim/blackout | `engineRunning()` | 3 s rule | **same rule**: `lastUpdate` = `m` of rpm, 3 s, rpm > 300 |
| `power.cpp` `cutPowerEngineOff` | `engineRunning()` | 3 s rule | **same rule** |
| `hub.cpp` `dataFresh` | `vehicleDataFresh()` | 3 s | same |
| `hub.cpp` `rpm`/`speed`/`throttle` | values | as is | as is (stale speed/throttle are 0) |
| `hub.cpp` `temp` | temp | number | **`null` unless `kFreshTemp`**: a stale temperature is never shown as 0 °C |
| STATUS_A flags bit 4 `rpmFresh` | rpm | — | `kFreshRpm` of the view |
| `vehicle.cpp` stale reset (3 s → 0) | rpm/speed/throttle | 3 s | same (consistent with the view) |
| `obd2.cpp` decode and getters | all | OBD2 only | not used (plan: no OBD) |
| `kline.cpp` | all | DS2 only | not used (plan: no K-line) |

The per-source decisions are pure functions in `src/vehicle/vehicle_rules.h`
(`vehicleFieldFresh`, `scaleFractionVehicle`, `shiftLight`, `tempKnown`, `parkedCut`) used by
`led_control.cpp`, `power.cpp` and `hub.cpp`, so the host tests exercise the code the
firmware runs. For modes 0…3 they return exactly what the old expressions returned.

## Identity, dispatcher, STATUS_A

- `deviceId` "L8-" + MAC (eFuse), `bootId` 16 bytes of `esp_random()` per boot, firmware
  version string. The stored panel id and address (§4.3) live in NVS behind `PanelStore`
  (host: a model) and are written when the core confirms its address.
- REQUESTs go to the **same** dispatcher as HTTP and USB: `l8proto::handleV1(...,
  Transport::Usb, true, ...)` — what `panel_node` already did in 027.
- STATUS_A: master, previewActive, ioOk, anyFault, rpmFresh (§7), revision/saved from
  settings, channel on/fault masks from the power snapshot.

## What the user sees

- Settings: `can.mode` 4 «Панель D10».
- `status` (hub): `canStatus` = `panel <phase> addr 0x.. <veh: fresh|stale|none> busoff N`;
  `dataFresh`, values and `temp: null` as above.
- Outputs, IGN, Master, `inhibit` and the current protections stay local (§15).

## `panel_node` and the rigs

`tools/panel_node.cpp` is rebuilt on `PanelMode` with a line-protocol `PanelPort` and the
real `l8proto`: the participant of `scripts/l8-panel-core.sh` and `vehicle-feed-e2e.sh` is
the mode code that goes into the firmware, not a separate harness. Its line protocol and
outputs stay the same.

## Criteria

| ID | Expected |
|---|---|
| D1 | `l8_v3` builds; sections and symbols compared with base `8849a5c`, every difference explained; host test: `modePlan(0…3)` = the old behaviour, `panel` false — no new path runs with old settings |
| D2 | host, model TWAI, `-Werror` ASan/UBSan: enter/exit, discovery and address, VEH and per-field freshness, STATUS_A, requests through the real dispatcher, bus-off and recovery, reboot (stored address) |
| D3 | each reader of the table: fresh / stale / no data. Stale temperature never 0 °C; rpm scale and shift light off on stale rpm; `cutPowerEngineOff` by the 3 s rule; other modes unchanged |
| D4 | rigs: `l8-panel-core.sh` and `vehicle-feed-e2e.sh` with `panel_node` built on `PanelMode` |
| D5 | all previous native suites of L8 and all suites of D10 unchanged |
| X | mutants: temperature read without its bit; rpm scale on stale rpm; panel mode active with old settings |
