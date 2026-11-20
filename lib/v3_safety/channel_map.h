#pragma once
#include <stdint.h>
namespace v3 {
struct ChannelPins { uint8_t power, data, ground, hardware; };
// User channels follow the +12 V contacts on J1. Hardware keeps the original
// schematic LED1..LED8 / PCA P00..P07 / GPIO11..18 numbering.
constexpr ChannelPins CHANNELS[8] = {
    {1,10,26,1}, {2,11,27,0}, {3,12,28,5}, {4,13,29,4},
    {6,15,31,3}, {7,16,32,2}, {8,17,33,7}, {9,25,34,6}
};
constexpr unsigned hardwareChannel(unsigned channel) { return channel<8 ? CHANNELS[channel].hardware : 8; }
constexpr uint8_t channelMask(unsigned channel) { return channel<8 ? uint8_t(1u<<hardwareChannel(channel)) : 0; }
constexpr int dataPin(unsigned channel) { return channel<8 ? 11+hardwareChannel(channel) : -1; }
template<typename T> void migrateLegacyChannels(T (&values)[8]) {
    T previous[8];
    for(unsigned i=0;i<8;++i) previous[i]=values[i];
    for(unsigned i=0;i<8;++i) values[i]=previous[hardwareChannel(i)];
}
}
