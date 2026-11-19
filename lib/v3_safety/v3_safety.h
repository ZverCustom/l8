#pragma once
#include <stdint.h>
#include <math.h>
#include <power_latch.h>
namespace v3 {
constexpr uint16_t SAFE = 0x2000; // only CAN_S high
inline float currentAmps(float volts) { return volts * 5450.0f / 1200.0f; }
inline uint16_t senseSelect(unsigned channel) {
    return channel < 8 ? uint16_t((1u << (8 + channel/2)) | ((channel & 1) << 12)) : 0;
}
inline bool supplyValid(float vbat) { return isfinite(vbat) && vbat >= 9.0f && vbat <= 16.5f; }
inline bool outputsAllowed(float vbat,bool ign,bool bench,bool inhibit,bool ready,bool latched) { return supplyValid(vbat) && (ign||bench) && !inhibit && ready && !latched; }
inline float inputFraction(float mv, int mode, float low, float high, float threshold) {
    if (mode == 1) return mv > threshold ? 1.f : 0.f;
    if (mode == 2) return mv < threshold ? 1.f : 0.f;
    float span = high-low;
    if (!isfinite(mv) || !isfinite(span) || span <= 0) return 0;
    float f = (mv * 0.0053f - low) / span;
    return f < 0 ? 0 : f > 1 ? 1 : f;
}
}
