#pragma once
#include <stdint.h>
#include <math.h>
namespace l8anim {
struct Clock {
    uint32_t last=0; bool started=false; double ticks=0;
    float advance(uint32_t now,int speed) {
        if(!started) { last=now; started=true; return 0; }
        uint32_t dt=now-last; last=now;
        if(speed<0) speed=0; if(speed>100) speed=100;
        ticks+=double(dt)/(150.0-1.34*speed);
        return dt/1000.f;
    }
};
struct Meter {
    float value=0, peak=0; uint32_t peakAt=0;
    void update(float target,float seconds,uint32_t now) {
        if(!isfinite(target)) target=0;
        target=fmaxf(-1,fminf(1,target));
        float tau=fabsf(target)>fabsf(value)?0.045f:0.30f;
        value+=(target-value)*(1-expf(-seconds/tau));
        if(value*peak<0) peak=0;
        if(fabsf(value)>=fabsf(peak)) { peak=value; peakAt=now; }
        else if(now-peakAt>=400) peak*=expf(-seconds/0.45f);
    }
};
}
