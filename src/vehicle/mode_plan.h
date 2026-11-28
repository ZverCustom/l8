// What each CAN mode does with the bus (task 036, docs/PANEL_MODE.md). Pure: no Arduino,
// no ESP-IDF; host-tested. Rows 0…3 are the behaviour vehicle.cpp had before the panel mode.
#pragma once

namespace vmode {

constexpr int kObd2 = 0, kBmw = 1, kSniffer = 2, kDs2 = 3, kPanel = 4;
constexpr int kBr500k = 0, kBr1M = 4;  // CanBitrate indices (can_bus.h)

struct Plan {
  bool listenOnly;    // TWAI listen-only
  int fixedBitrate;   // -1: the user's selection (0 = auto → 500k), else this index
  bool autoBitrate;   // the auto-bitrate scan may run (only when the selection is auto)
  bool obd;           // OBD2 polling
  bool kline;         // K-line (DS2)
  bool readFrames;    // vehicleTick reads TWAI frames itself (BMW decode / sniffer)
  bool panel;         // panel mode (§15)
};

inline Plan plan(int mode) {
  switch (mode) {
    case kObd2:
      return {false, -1, false, true, false, false, false};
    case kDs2:
      return {true, -1, false, false, true, false, false};
    case kPanel:
      return {true, kBr1M, false, false, false, false, true};  // listen first (A5, R92)
    default:  // BMW, sniffer and any other value: listen-only, read frames
      return {true, -1, true, false, false, true, false};
  }
}

// The bitrate index a restart uses: fixed by the mode, else the selection (0 = auto → 500k).
inline int bitrateFor(int mode, int sel) {
  Plan p = plan(mode);
  if (p.fixedBitrate >= 0) return p.fixedBitrate;
  return sel ? sel - 1 : kBr500k;
}

// A bitrate setting change restarts the driver (A5 rev 2, R94): never in panel mode, where only
// PanelMode switches TWAI; the saved selection applies when the mode is left.
inline bool bitrateChangeRestarts(int mode) { return !plan(mode).panel; }

}  // namespace vmode
