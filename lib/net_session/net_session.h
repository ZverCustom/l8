#pragma once
#include <stdint.h>
namespace l8net {
// At most three explicit begin calls after a successful boot discovery.
// Seeing an AP client permanently closes this boot retry budget; client departure
// does not restart it. Manual reconnects do not use this budget.
struct BootAttempts {
    static constexpr uint8_t MAX_ATTEMPTS=3;
    uint8_t attempts=0;
    bool allowed=false;
    void discovered() { allowed=true; attempts=0; }
    void started() { if(allowed && attempts<MAX_ATTEMPTS) ++attempts; }
    void stop() { allowed=false; }
    void observeClients(bool present) { if(present) stop(); }
    bool retryAllowed() const { return allowed && attempts>0 && attempts<MAX_ATTEMPTS; }
};
// Each individual attempt keeps its full association/DHCP deadline. The Arduino core can retry its
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
