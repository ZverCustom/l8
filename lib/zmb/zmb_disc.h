// Module side of ZMB discovery and addresses (contract v6 §4.1-§4.2), portable C++17,
// sans-IO: frames in/out, time as wrapping uint32 ms, randomness from a caller function
// (deterministic in tests). Not part of the firmware CAN path yet.
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "zmb.h"

namespace zmb {

namespace sfunc {
constexpr uint8_t kPanelHb = 0x04, kStatusA = 0x18, kBeacon = 0x1C, kDiscover = 0x3C, kAssign = 0x3D, kAnnounce = 0x3E;
}

uint32_t mac20(const uint8_t mac[6]);
// Identity ID: prio, func, low 20 bits of the MAC.
uint32_t identityId(uint8_t prio, uint8_t func, const uint8_t mac[6]);

// Uniform random uint32 from the caller (seeded per node; any source in firmware).
using RandFn = uint32_t (*)(void* ctx);

// Rules 1-7 of §4.2:
//  1. no addressed frames until an ASSIGN with our MAC and the low byte of the panelGen of
//     the last PANEL_HB, in this boot;
//  2. wantAddr = stored address if the stored panelId equals PANEL_HB's, else 0;
//  3. ANNOUNCE (single-shot): first 0..200 ms after the first PANEL_HB of a panelGen;
//     lost arbitration 1..20 ms; bus error 50..250 ms; sent without ASSIGN 1000 + 0..250 ms;
//     5 bus errors in a row count `announceErrors`;
//  4. BEACON (single-shot) every 1000 +- 125 ms; lost arbitration 1..20 ms; bus error: next
//     period (`beaconErrors`);
//  5. our address in another MAC's BEACON: stop addressed frames, forget the confirmation,
//     ANNOUNCE again with wantAddr 0;
//  6. RELEASE (ASSIGN addr 0): forget the address, back to rule 1;
//  7. a new panelGen (PANEL_HB or DISCOVER): abort everything, forget the confirmation (the
//     address stays as wantAddr), rule 3.
// Every loss of the confirmation increments generation(): the owner then drops its
// transport state (assemblies, sender) before using the next address.
class ModuleDiscovery {
 public:
  ModuleDiscovery(const uint8_t mac[6], uint8_t devType, RandFn rand, void* randCtx);
  // What the module kept from an earlier boot (NVS later): panelId and address.
  void setStored(uint32_t panelId, uint8_t addr) { storedPanelId_ = panelId; storedAddr_ = addr; }
  // Low byte of the current bootTag, carried by BEACON.
  void setBootLo(uint8_t b) { bootLo_ = b; }

  // Feeds every received frame; returns true if it was a discovery/panel frame.
  bool onFrame(const CanFrame& f, uint32_t now);
  // The single-shot frame (ANNOUNCE or BEACON) to transmit now, if any.
  bool poll(uint32_t now, CanFrame& out);
  // Result of the frame from poll(): sent, lost arbitration, or bus error.
  void sent(uint32_t now);
  void lost(uint32_t now);
  void busError(uint32_t now);
  // Back to ANNOUNCE (e.g. no HELLO_ACK in time), keeping the address as wantAddr.
  void restart(uint32_t now);

  bool assigned() const { return assigned_; }
  uint8_t addr() const { return assigned_ ? addr_ : 0; }
  uint32_t panelId() const { return panelId_; }
  uint32_t panelGen() const { return panelGen_; }
  bool havePanel() const { return havePanel_; }
  uint32_t generation() const { return generation_; }
  uint8_t storedAddr() const { return storedAddr_; }
  uint32_t announceErrors = 0, beaconErrors = 0;

 private:
  uint32_t rnd(uint32_t lo, uint32_t hi);  // inclusive
  void forget(uint32_t now, bool keepWant, uint32_t delayLo, uint32_t delayHi);
  void onPanel(uint32_t panelId, uint32_t panelGen, uint32_t now);
  uint8_t mac_[6];
  uint8_t devType_;
  RandFn rand_;
  void* randCtx_;
  uint32_t storedPanelId_ = 0;
  uint8_t storedAddr_ = 0;
  bool havePanel_ = false, assigned_ = false;
  uint32_t panelId_ = 0, panelGen_ = 0;
  uint8_t addr_ = 0, want_ = 0;
  bool announceDue_ = false;
  uint32_t announceAt_ = 0;
  uint32_t beaconAt_ = 0;
  uint8_t busErrorsInRow_ = 0;
  uint8_t bootLo_ = 0;
  enum class Last : uint8_t { None, Announce, Beacon } last_ = Last::None;
  uint32_t generation_ = 0;
};

}  // namespace zmb
