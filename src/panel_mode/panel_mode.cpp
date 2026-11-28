#include "panel_mode.h"

#include <new>
#include <string.h>

namespace panel_mode {

using panel_core::SingleShot;

PanelMode::PanelMode(PanelPort& port, PanelStore& store, const Host& host) : port_(port), store_(store), host_(host) {}

PanelMode::~PanelMode() { exit(); }

void PanelMode::txThunk(void* c, const zmb::CanFrame& f, bool single) {
  PanelMode& m = *static_cast<PanelMode*>(c);
  if (single) m.pendingSingle_ = true;
  // listen (A5): nothing is transmitted; a single-shot frame is answered in service(), never
  // from inside the core
  bool blocked = m.listen_;
  if (blocked || !m.port_.send(f, single)) {
    if (blocked) m.c_.txBlocked++;
    else m.c_.sendFailed++;
    if (single) {
      m.forced_ = true;
      m.forcedOutcome_ = SingleShot::Lost;
    }
    return;
  }
  m.c_.sent++;
}

void PanelMode::enter(const uint8_t mac[6], uint8_t devType, const uint8_t bootId[16], const char* deviceId,
                      const char* firmware, uint8_t hwRev) {
  exit();
  panel_core::Identity id;
  memcpy(id.mac, mac, 6);
  id.devType = devType;
  memcpy(id.bootId, bootId, 16);
  id.deviceId = deviceId;
  id.firmware = firmware;
  id.hwRev = hwRev;
  uint32_t pid = 0;
  uint8_t addr = 0;
  if (store_.load(pid, addr)) {
    id.storedPanelId = pid;
    id.storedAddr = addr;
  }
  savedId_ = pid;
  savedAddr_ = addr;
  panel_core::Buffers b{rxbuf_, sizeof rxbuf_, respbuf_, sizeof respbuf_, tids_, sizeof tids_ / sizeof tids_[0]};
  // the core calls us; dispatch/statusA/log go on to the host with its own context
  panel_core::CoreIo io;
  io.ctx = this;
  io.tx = txThunk;
  io.dispatch = [](void* c, const uint8_t* req, size_t len, uint8_t* resp, size_t cap) -> size_t {
    PanelMode& m = *static_cast<PanelMode*>(c);
    return m.host_.dispatch ? m.host_.dispatch(m.host_.ctx, req, len, resp, cap) : 0;
  };
  io.statusA = [](void* c, uint8_t* flags, uint16_t* rev, uint16_t* saved, uint8_t* chOn, uint8_t* chFault) {
    PanelMode& m = *static_cast<PanelMode*>(c);
    if (m.host_.statusA) m.host_.statusA(m.host_.ctx, flags, rev, saved, chOn, chFault);
  };
  io.log = [](void* c, const char* t) {
    PanelMode& m = *static_cast<PanelMode*>(c);
    if (m.host_.log) m.host_.log(m.host_.ctx, t);
  };
  stamp_ = panel_rx::QueueStamp();
  overrunSeen_ = port_.overrunCount();
  pendingSingle_ = forced_ = false;
  // A5: every entry starts listen-only; bus-off history starts over
  port_.setListenOnly(true);
  listen_ = true;
  reason_ = Listen::Entry;
  hbCount_ = 0;
  busOffRun_ = 0;
  holdUntil_ = host_.now(host_.ctx);
  core_ = new (coreMem_) panel_core::PanelCore(id, b, io, host_.rand, host_.randCtx);
}

void PanelMode::exit() {
  if (!core_) return;
  core_->~PanelCore();
  core_ = nullptr;
  pendingSingle_ = forced_ = false;
}

void PanelMode::rx(int budget) {
  for (int i = 0; core_ && i < budget; i++) {
    // §14.3: t0 first, then the emptiness check, then the read; the frame uses this anchor
    uint32_t t0 = host_.now(host_.ctx);
    bool empty = port_.rxQueueEmpty();
    uint32_t ov = port_.overrunCount();
    bool grew = ov != overrunSeen_;
    overrunSeen_ = ov;
    if (grew) c_.overrunVoids++;
    panel_rx::Poll p = stamp_.poll(panel_rx::Snapshot::at(t0, empty, grew));
    zmb::CanFrame f;
    if (!port_.read(f)) break;
    uint32_t tRead = host_.now(host_.ctx);
    uint32_t ov2 = port_.overrunCount();
    bool grew2 = ov2 != overrunSeen_;
    overrunSeen_ = ov2;
    if (grew2) c_.overrunVoids++;
    panel_rx::Stamp st = stamp_.read(p, tRead, grew2);
    c_.frames++;
    noteHb(f, tRead);
    core_->onFrame(f, st, tRead);
  }
}

static bool before(uint32_t a, uint32_t b) { return int32_t(a - b) < 0; }

void PanelMode::noteHb(const zmb::CanFrame& f, uint32_t t) {
  if (!f.extended || f.rtr || f.dlc != 8 || f.id != panel_rx::kIdPanelHb) return;
  uint32_t id = uint32_t(f.data[0]) << 24 | uint32_t(f.data[1]) << 16 | uint32_t(f.data[2]) << 8 | f.data[3];
  uint32_t gen = uint32_t(f.data[4]) << 24 | uint32_t(f.data[5]) << 16 | uint32_t(f.data[6]) << 8 | f.data[7];
  if (id == 0) return;
  if (!listen_) {
    lastHb_ = t;
    return;
  }
  if (before(t, holdUntil_)) return;  // pause after bus-off: not counted
  bool same = hbCount_ && id == hbId_ && gen == hbGen_;
  if (same && t - hbLast_ < kHbMinMs) return;  // a retransmission of the same period: ignored
  if (same && t - hbLast_ <= kHbGapMs) {
    hbCount_++;
  } else {
    hbCount_ = 1;
    hbId_ = id;
    hbGen_ = gen;
  }
  hbLast_ = t;
  if (hbCount_ < kHbNeeded) return;
  // a D10 panel is on this bus at this bitrate: normal mode (A5)
  port_.setListenOnly(false);
  listen_ = false;
  hbCount_ = 0;
  activeSince_ = lastHb_ = t;
  c_.activations++;
}

void PanelMode::goListen(uint32_t now, Listen why) {
  port_.setListenOnly(true);
  listen_ = true;
  reason_ = why;
  hbCount_ = 0;
  (void)now;
}

uint32_t PanelMode::retryInMs(uint32_t now) const { return before(now, holdUntil_) ? holdUntil_ - now : 0; }

void PanelMode::service() {
  if (!core_) return;
  uint32_t now = host_.now(host_.ctx);
  if (!listen_ && port_.busOff()) {
    c_.busOffs++;
    if (pendingSingle_) {
      pendingSingle_ = forced_ = false;
      core_->singleShotResult(SingleShot::BusError, now);
    }
    busOffRun_++;
    bool limit = busOffRun_ > kBusOffLimit;
    holdUntil_ = now + (limit ? kHoldMs : (1000u << (busOffRun_ - 1)));
    goListen(now, limit ? Listen::Limit : Listen::BusOff);
  } else if (!listen_ && now - lastHb_ > kHbLossMs) {
    goListen(now, Listen::NoHb);
  } else if (!listen_ && busOffRun_ && now - activeSince_ >= kCleanMs) {
    busOffRun_ = 0;  // a minute of normal mode without bus-off
  }
  if (!pendingSingle_) return;
  SingleShot r;
  if (forced_) {
    pendingSingle_ = forced_ = false;
    core_->singleShotResult(forcedOutcome_, now);
  } else if (port_.singleShotDone(r)) {
    pendingSingle_ = false;
    core_->singleShotResult(r, now);
  }
}

void PanelMode::tick() {
  if (!core_) return;
  rx();
  service();
  core_->tick(host_.now(host_.ctx));
  if (core_->ready()) {
    uint32_t pid = core_->discovery().panelId();
    uint8_t a = core_->addr();
    if (a != 0 && (pid != savedId_ || a != savedAddr_)) {
      store_.save(pid, a);
      savedId_ = pid;
      savedAddr_ = a;
    }
  }
}

panel_core::VehicleView PanelMode::view(uint32_t now) const {
  return core_ ? core_->panelVehicle(now) : panel_core::VehicleView{};
}

const char* PanelMode::phase() const {
  if (!core_) return "off";
  if (listen_) return "listen";
  if (core_->ready()) return "ready";
  return core_->addr() ? "hello" : "unassigned";
}

}  // namespace panel_mode
