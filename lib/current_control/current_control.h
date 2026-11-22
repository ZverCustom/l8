#pragma once
#include <stdint.h>
#include <math.h>
namespace l8power {
// Real measured current, independent of LED model/voltage and requested brightness.
struct Dimmer {
    float scale=1; uint32_t last=0; bool sampled=false;
    void update(uint32_t now,float amps,float limit) {
        float dt=sampled?fminf((now-last)/1000.f,.1f):.005f; sampled=true; last=now;
        if(limit<=0) { scale=1; return; }
        const float target=limit*.90f;
        if(amps>target) scale*=1-fminf(1.f,dt/.04f)*(1-target/amps);
        else if(amps<limit*.82f) scale+=dt*.10f;
        scale=fmaxf(0.f,fminf(1.f,scale));
    }
};
struct Channel {
    enum Fault:uint8_t { None=0, Overcurrent=1, Switch=2, Spike=3, InvalidSample=4 };
    Dimmer dimmer;
    Fault fault=None; uint8_t retries=0; bool waiting=false;
    uint32_t tripped=0, overAt=0, healthyAt=0;
    bool over=false, healthy=false;
    void reset() { *this=Channel{}; }
    void off() { over=false; healthy=false; dimmer.sampled=false; }
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
        if(volts>=1.6f) { trip(Switch,now,false); return; }
        if(amps>=fmaxf(limit*2.f,limit+1.f)) { trip(Spike,now,false); return; }
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
