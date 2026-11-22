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
// Mirrors Arduino 2.0.17 first_connect. Called for EVERY disconnect in the
// event task before queuing, so queue overflow cannot restore a consumed retry.
struct CoreRetry {
    bool available=true;
    bool onDisconnect(bool voluntary) {
        if(voluntary || !available) return false;
        available=false; return true;
    }
};
// Deadlines cover hung attempts; disconnects resolve failures sooner. Arduino's
// one internal retry receives a fresh association deadline without another begin here.
struct Session {
    enum Phase { Idle, Scan, Associating, Dhcp, Online, Stopping, Failed } phase=Idle;
    uint32_t since=0;
    void enter(Phase value,uint32_t now) { if(phase!=value) { phase=value; since=now; } }
    void stopping(uint32_t now) { phase=Stopping; since=now; }
    bool restartReady(uint32_t now) const { return phase==Stopping && now-since>=2000; }
    void disconnected(uint32_t now,bool voluntary,bool coreWillRetry) {
        if(phase!=Associating && phase!=Dhcp) return;
        // The core's disconnect()/begin() may emit ASSOC_LEAVE too. It must
        // neither consume its one retry nor fail the attempt it just started.
        if(voluntary) return;
        if(coreWillRetry) { phase=Associating; since=now; }
        else enter(Failed,now);
    }
    bool expired(uint32_t now) const {
        uint32_t limit=phase==Scan?5000:phase==Associating?45000:phase==Dhcp?60000:0;
        return limit && now-since>=limit;
    }
};
}
