// The decisions of the vehicle data readers (task 036, docs/PANEL_MODE.md): one place for
// led_control, power and hub, host-tested. In panel mode a field is current by its own
// §14.4 freshness bit; in the other modes by the existing one 3 s stamp — unchanged.
#pragma once

#include <stdint.h>

namespace vrules {

// field: 0 rpm, 1 speed, 2 throttle, 3 temp (bits of panel_core::kFresh*)
inline bool fieldFresh(bool panel, uint8_t freshMask, bool dataFresh3s, int field) {
  return panel ? ((freshMask >> field) & 1) != 0 : dataFresh3s;
}

// A temperature may be read: other modes always (as before), panel mode only when fresh.
inline bool tempKnown(bool panel, uint8_t freshMask) { return panel ? (freshMask & 8) != 0 : true; }

// Scale sources 0 (rpm, also the default), 1 (speed), 2 (throttle) as a fraction.
inline float vehicleScale(int source, bool fresh, uint16_t rpm, uint8_t speed, uint8_t throttle, int maxSpeed,
                          int minRpm, int maxRpm) {
  if (!fresh) return 0;
  switch (source) {
    case 1:
      return maxSpeed > 0 ? float(speed) / float(maxSpeed) : 0;
    case 2:
      return float(throttle) / 100.0f;
    default: {
      int span = maxRpm - minRpm;
      if (span <= 0) return 0;
      float f = float(int(rpm) - minRpm) / float(span);
      return f > 0 ? f : 0.f;
    }
  }
}

inline bool shiftLight(bool rpmFresh, uint16_t rpm, int shiftRpm, int maxRpm) {
  int threshold = shiftRpm > 0 ? shiftRpm : maxRpm;
  return rpmFresh && int(rpm) >= threshold;
}

// cutPowerEngineOff: parked = the setting, engine-off mode 2, engine not running (3 s rule).
inline bool parkedCut(bool cutPowerEngineOff, int engineOff, bool engineRunning) {
  return cutPowerEngineOff && engineOff == 2 && !engineRunning;
}

}  // namespace vrules
