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
  if (m.busOff_ || !m.port_.send(f, single)) {
    // not on the bus: a single-shot frame is answered in service(), never from inside the core
    m.c_.sendFailed++;
    if (single) {
      m.forced_ = true;
      m.forcedOutcome_ = m.busOff_ ? SingleShot::BusError : SingleShot::Lost;
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
  busOff_ = port_.busOff();
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
    core_->onFrame(f, st, tRead);
  }
}

void PanelMode::service() {
  if (!core_) return;
  uint32_t now = host_.now(host_.ctx);
  bool off = port_.busOff();
  if (off && !busOff_) {
    c_.busOffs++;
    if (pendingSingle_) {
      pendingSingle_ = forced_ = false;
      core_->singleShotResult(SingleShot::BusError, now);
    }
    port_.recover();
  } else if (!off && busOff_) {
    c_.recoveries++;
  }
  busOff_ = off;
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
  if (core_->ready()) return "ready";
  return core_->addr() ? "hello" : "unassigned";
}

}  // namespace panel_mode
