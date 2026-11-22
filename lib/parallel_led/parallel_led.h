#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>
namespace l8led {
// 3.2 MHz / 4 samples = 800 kbit/s. T0H=312.5 ns, T1H=625 ns.
// 1024 zero samples give 320 us reset at BOTH ends of a complete frame.
constexpr size_t RESET_SAMPLES=1024;
constexpr size_t samples(size_t leds) { return 2*RESET_SAMPLES+leds*24*4; }
inline size_t encode(uint8_t* out,size_t capacity,const uint8_t* const strips[8],const int counts[8],const uint8_t masks[8],uint8_t allowed,uint8_t brightness) {
    int longest=0;
    for(int s=0;s<8;++s) { if(counts[s]<0 || counts[s]>1000) return 0; if(counts[s]>longest)longest=counts[s]; }
    size_t length=samples(longest); if(capacity<length)return 0;
    memset(out,0,length); size_t pos=RESET_SAMPLES;
    for(int pixel=0;pixel<longest;++pixel) for(int component=0;component<3;++component) {
        uint8_t active=0,values[8]={};
        for(int s=0;s<8;++s) if(pixel<counts[s] && (allowed&masks[s])) {
            if(!strips[s]) return 0;
            active|=masks[s]; values[s]=(uint16_t(strips[s][pixel*3+component])*brightness+127)/255;
        }
        for(int bit=7;bit>=0;--bit) {
            uint8_t ones=0; for(int s=0;s<8;++s) if(values[s]&(1u<<bit))ones|=masks[s];
            out[pos++]=active; out[pos++]=ones&active; out[pos++]=0; out[pos++]=0;
        }
    }
    return length;
}
}
