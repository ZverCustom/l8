#pragma once
#include <stdint.h>

namespace l8net {
enum class Phase { Idle, Connecting, Address, Online, Failed, Stopping };
enum class Action { Wait, Start, Cancel };
class HttpActivity {
    bool seen=false;
    uint32_t last=0;
public:
    void touch(uint32_t now) { seen=true; last=now; }
    bool active(uint32_t now) {
        if(seen && uint32_t(now-last)>=60000) seen=false;
        return seen;
    }
};
class Retry {
public:
    Phase phase=Phase::Idle;
    uint32_t since=0;
    bool coreRetryAvailable=true;
    void started(uint32_t now) { phase=Phase::Connecting; since=now; }
    void associated(uint32_t now) { if(phase!=Phase::Address) { phase=Phase::Address; since=now; } }
    void online(uint32_t now) { phase=Phase::Online; since=now; }
    void failed(uint32_t now) { phase=Phase::Failed; since=now; }
    void disconnected(uint32_t now,bool voluntary) {
        // Arduino-ESP32 2.0.17 retries the first non-voluntary failure once,
        // even with setAutoReconnect(false). That attempt is already running.
        if(!voluntary && coreRetryAvailable) { coreRetryAvailable=false; started(now); }
        else failed(now);
    }
    void stopping(uint32_t now) { phase=Phase::Stopping; since=now; }
    void reconcileOffline(uint32_t now,bool associatedNow) {
        if(associatedNow) associated(now);
        else if(phase==Phase::Online) failed(now);
    }
    Action next(uint32_t now,bool ap,bool httpActive) const {
        uint32_t age=now-since;
        if(phase==Phase::Connecting) return age>=45000 ? Action::Cancel : Action::Wait;
        if(phase==Phase::Address) return age>=60000 ? Action::Cancel : Action::Wait;
        if(phase==Phase::Online || phase==Phase::Stopping) return Action::Wait;
        // Keep the AP radio available while someone is using the configurator.
        if(ap && httpActive) return Action::Wait;
        return age>=(ap?30000u:10000u) ? Action::Start : Action::Wait;
    }
};
}
