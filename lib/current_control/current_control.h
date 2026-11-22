#pragma once
#include <stdint.h>
#include <math.h>
namespace l8power {
// Real measured current, independent of LED model/voltage and requested brightness.
struct Dimmer {
    float scale=1; uint32_t last=0; bool sampled=false, surge=false;
    void update(uint32_t now,float amps,float limit) {
        float dt=sampled?fminf((now-last)/1000.f,.1f):.005f; sampled=true; last=now;
        if(limit<=0) { scale=1; surge=false; return; }
        const float target=limit*.90f;
        if(amps<limit) surge=false;
        // A bright animation edge can demand 2x the limit. Make a fast initial
        // correction, then let normal feedback settle; do not classify it as a
        // short. One correction per surge avoids repeatedly multiplying against
        // an unchanged measurement while the next LED frame is still pending.
        if(amps>=2*limit && !surge) { scale*=target/amps; surge=true; }
        else if(amps>target) scale*=1-fminf(1.f,dt/.04f)*(1-target/amps);
        else if(amps<limit*.82f) scale+=dt*.10f;
        scale=fmaxf(0.f,fminf(1.f,scale));
    }
};
struct Channel {
    // Fault code 3 (instantaneous current Spike) retired; current alone is not a short diagnosis.
    enum Fault:uint8_t { None=0, Overcurrent=1, Switch=2, InvalidSample=4 };
    static constexpr uint32_t INRUSH_MS=10, SWITCH_MS=50;
    Dimmer dimmer;
    Fault fault=None; uint8_t retries=0; bool waiting=false;
    uint32_t tripped=0, overAt=0, healthyAt=0, enabledAt=0, switchAt=0;
    bool over=false, healthy=false, switchHigh=false, poweringUp=false;
    void reset() { *this=Channel{}; }
    void off() { over=false; healthy=false; switchHigh=false; poweringUp=false; dimmer.sampled=false; dimmer.surge=false; }
    // Called only after the physical IN write succeeds, on each OFF -> ON.
    void enabled(uint32_t now) { off(); enabledAt=now; poweringUp=true; }
    bool ready(uint32_t now,bool autoRetry,bool permitted) {
        if(!autoRetry) waiting=false;
        if(waiting && permitted && now-tripped>=delayMs()) {
            waiting=false; fault=None; ++retries; off();
            // Reapply a bounded brightness at recovery; hardware diagnostic is
            // still sampled before any overload retry may be scheduled again.
            dimmer.scale=fminf(dimmer.scale,.25f);
        }
        return fault==None;
    }
    uint32_t delayMs() const { return retries==0?1000:retries==1?5000:30000; }
    uint32_t remaining(uint32_t now) const { return waiting && now-tripped<delayMs()?delayMs()-(now-tripped):0; }
    void trip(Fault reason,uint32_t now,bool autoRetry) {
        fault=reason; tripped=now; over=false; healthy=false;
        waiting=reason==Overcurrent && autoRetry && retries<3;
    }
    void sample(uint32_t now,float amps,float volts,float limit,bool autoRetry) {
        if(fault!=None) return;
        if(!isfinite(amps)||!isfinite(volts)||!isfinite(limit)||limit<=0) { trip(InvalidSample,now,false); return; }
        if(poweringUp && now-enabledAt<INRUSH_MS) return;
        poweringUp=false;
        if(volts>=1.6f) {
            // High IS may be load current or a switch diagnostic. Give a bright
            // frame one reduction while retaining the full 50 ms fault filter.
            // 7.3 A is the nominal current at the 1.6 V boundary, not a measured
            // fault current. Never repeatedly dim against the same pending frame.
            over=false; healthy=false;
            if(!switchHigh) {
                switchHigh=true; switchAt=now;
                dimmer.scale*=fminf(1.f,limit*.9f/7.3f);
            }
            if(now-switchAt>=SWITCH_MS) trip(Switch,now,false);
            return;
        }
        switchHigh=false;
        // Every valid current sample, including a large step, gets the same
        // 100 ms regulation opportunity and bounded overload recovery below.
        dimmer.update(now,amps,limit);
        if(amps>limit) {
            healthy=false;
            if(!over) { over=true; overAt=now; }
            if(now-overAt>=100) trip(Overcurrent,now,autoRetry);
        } else {
            over=false;
            if(!healthy) { healthy=true; healthyAt=now; }
            if(now-healthyAt>=60000) retries=0;
        }
    }
};
}
