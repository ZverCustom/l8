// L8 panel mode (v6 §15, task 036, docs/PANEL_MODE.md): PanelCore over a PanelPort with the
// §14.3 queue stamp, single-shot outcomes, bus-off recovery and the stored address. No
// Arduino and no ESP-IDF here: the firmware glue (panel_glue.cpp) and the host rig/tests
// use the same class.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "panel_core.h"
#include "panel_port.h"
#include "panel_rx.h"

namespace panel_mode {

struct Host {
  void* ctx = nullptr;
  uint32_t (*now)(void* ctx) = nullptr;  // L8 ms, the clock of the queue stamp
  size_t (*dispatch)(void* ctx, const uint8_t* req, size_t len, uint8_t* resp, size_t cap) = nullptr;
  void (*statusA)(void* ctx, uint8_t* flags, uint16_t* rev, uint16_t* saved, uint8_t* chOn, uint8_t* chFault) = nullptr;
  void (*log)(void* ctx, const char* text) = nullptr;
  zmb::RandFn rand = nullptr;
  void* randCtx = nullptr;
};

struct Counters {
  uint32_t frames = 0, sent = 0, sendFailed = 0, txBlocked = 0, busOffs = 0, activations = 0, overrunVoids = 0;
};

// v6 addendum A5 (R92): listen first, normal after 3 PANEL_HB, back on loss or bus-off.
constexpr uint8_t kHbNeeded = 3;
// kHbMinMs (A5 rev 2): counted PANEL_HB at least this far apart — three periods, not three
// retransmissions of one frame
constexpr uint32_t kHbGapMs = 300, kHbMinMs = 50, kHbLossMs = 1000, kHoldMs = 60000, kCleanMs = 60000;
constexpr uint8_t kBusOffLimit = 5;  // pauses 1, 2, 4, 8, 16 s; then held kHoldMs
enum class Listen : uint8_t { Entry, NoHb, BusOff, Limit };

class PanelMode {
 public:
  PanelMode(PanelPort& port, PanelStore& store, const Host& host);
  ~PanelMode();

  // A new boot of the panel mode: a new core with this identity and the stored address.
  void enter(const uint8_t mac[6], uint8_t devType, const uint8_t bootId[16], const char* deviceId,
             const char* firmware, uint8_t hwRev);
  void exit();
  bool active() const { return core_ != nullptr; }

  // Receive step(s): snapshot, read, t_read, in that order (§14.3); budget frames at most.
  void rx(int budget = 64);
  // Single-shot outcome and bus-off handling.
  void service();
  // Everything of one loop pass: rx, service, core timers, the stored address.
  void tick();

  panel_core::VehicleView view(uint32_t now) const;
  panel_core::PanelCore* core() { return core_; }
  const panel_core::PanelCore* core() const { return core_; }
  const Counters& counters() const { return c_; }
  bool listening() const { return listen_; }
  Listen listenReason() const { return reason_; }
  uint8_t hbCount() const { return hbCount_; }
  uint8_t busOffRun() const { return busOffRun_; }
  // ms until PANEL_HB counting resumes (0: counting now)
  uint32_t retryInMs(uint32_t now) const;
  // "off" | "listen" | "unassigned" | "hello" | "ready"
  const char* phase() const;

 private:
  static void txThunk(void* c, const zmb::CanFrame& f, bool single);
  PanelPort& port_;
  PanelStore& store_;
  Host host_;
  panel_core::PanelCore* core_ = nullptr;
  alignas(panel_core::PanelCore) uint8_t coreMem_[sizeof(panel_core::PanelCore)];
  panel_rx::QueueStamp stamp_;
  uint32_t overrunSeen_ = 0;
  void noteHb(const zmb::CanFrame& f, uint32_t t);
  void goListen(uint32_t now, Listen why);
  bool pendingSingle_ = false;
  bool listen_ = true;
  Listen reason_ = Listen::Entry;
  uint8_t hbCount_ = 0, busOffRun_ = 0;
  uint32_t hbId_ = 0, hbGen_ = 0, hbLast_ = 0, lastHb_ = 0, activeSince_ = 0, holdUntil_ = 0;
  bool forced_ = false;  // a single-shot frame that never reached the bus
  panel_core::SingleShot forcedOutcome_ = panel_core::SingleShot::Lost;
  uint32_t savedId_ = 0;
  uint8_t savedAddr_ = 0;
  Counters c_;
  uint8_t rxbuf_[zmb::kMaxMessage];
  uint8_t respbuf_[zmb::kMaxMessage];
  zmb::TidEntry tids_[256];
};

}  // namespace panel_mode
