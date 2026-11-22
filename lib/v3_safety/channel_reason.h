#pragma once
#include <stdint.h>
namespace v3 {
// Why a channel is (not) on, computed by the power task from the same inputs it
// used for the output decision. Diagnostic only: never feeds back into switching.
enum class Reason:uint8_t { On, Io, Fault, Inhibit, NoVbat, Ign, Master, Disabled, Engine, Retry };
struct ReasonInputs {
    bool ioOk, faultLatched, retryPending, inhibit, supplyOk, ignOk, master, enabled, engineCut, on;
};
// Contract priority (docs/protocol.md section 6): first matching row wins.
inline Reason channelReason(const ReasonInputs& x) {
    if(!x.ioOk) return Reason::Io;
    if(x.faultLatched && !x.retryPending) return Reason::Fault;
    if(x.inhibit) return Reason::Inhibit;
    if(!x.supplyOk) return Reason::NoVbat;
    if(!x.ignOk) return Reason::Ign;
    if(!x.master) return Reason::Master;
    if(!x.enabled) return Reason::Disabled;
    if(x.engineCut) return Reason::Engine;
    if(x.retryPending) return Reason::Retry;
    // All permits hold; a channel that is still off has a fault decided this cycle.
    return x.on ? Reason::On : Reason::Fault;
}
inline const char* reasonName(Reason r) {
    switch(r) {
        case Reason::On: return "on"; case Reason::Io: return "io"; case Reason::Fault: return "fault";
        case Reason::Inhibit: return "inhibit"; case Reason::NoVbat: return "novbat"; case Reason::Ign: return "ign";
        case Reason::Master: return "master"; case Reason::Disabled: return "disabled";
        case Reason::Engine: return "engine"; case Reason::Retry: return "retry";
    }
    return "unknown";
}
}
