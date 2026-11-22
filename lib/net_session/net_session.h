#pragma once
#include <stdint.h>
namespace l8net {
class HttpActivity {
    bool seen=false; uint32_t last=0;
public:
    void touch(uint32_t now) { seen=true; last=now; }
    bool active(uint32_t now) { if(seen && now-last>=60000) seen=false; return seen; }
};
// One explicit session: no reconnect timer. The Arduino core can retry its
// first failed association internally once; that remains within this deadline.
struct Session {
    enum Phase { Idle, Scan, Associating, Dhcp, Online, Stopping } phase=Idle;
    uint32_t since=0;
    void enter(Phase value,uint32_t now) { if(phase!=value) { phase=value; since=now; } }
    bool expired(uint32_t now) const {
        uint32_t limit=phase==Scan?5000:phase==Associating?45000:phase==Dhcp?60000:0;
        return limit && now-since>=limit;
    }
};
}
