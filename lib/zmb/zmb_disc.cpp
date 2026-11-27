#include "zmb_disc.h"

#include <string.h>

namespace zmb {

uint32_t mac20(const uint8_t mac[6]) { return (uint32_t(mac[3] & 0x0F) << 16) | (uint32_t(mac[4]) << 8) | mac[5]; }

uint32_t identityId(uint8_t prio, uint8_t func, const uint8_t mac[6]) {
  return (uint32_t(prio & 7) << 26) | (uint32_t(func & 0x3F) << 20) | mac20(mac);
}

static uint32_t be32(const uint8_t* b) { return (uint32_t(b[0]) << 24) | (uint32_t(b[1]) << 16) | (uint32_t(b[2]) << 8) | b[3]; }

ModuleDiscovery::ModuleDiscovery(const uint8_t mac[6], uint8_t devType, RandFn rand, void* randCtx)
    : devType_(devType), rand_(rand), randCtx_(randCtx) {
  memcpy(mac_, mac, 6);
}

uint32_t ModuleDiscovery::rnd(uint32_t lo, uint32_t hi) { return lo + rand_(randCtx_) % (hi - lo + 1); }

void ModuleDiscovery::forget(uint32_t now, bool keepWant, uint32_t delayLo, uint32_t delayHi) {
  if (assigned_) generation_++;
  assigned_ = false;
  want_ = keepWant ? want_ : 0;
  announceDue_ = havePanel_;
  announceAt_ = now + rnd(delayLo, delayHi);
  busErrorsInRow_ = 0;
}

void ModuleDiscovery::restart(uint32_t now) { forget(now, true, 0, 200); }

void ModuleDiscovery::onPanel(uint32_t panelId, uint32_t panelGen, uint32_t now) {
  if (havePanel_ && panelGen == panelGen_ && panelId == panelId_) return;
  // first PANEL_HB of this panelGen (rule 3) or a new panelGen (rule 7)
  if (havePanel_ || assigned_) generation_++;
  bool keep = assigned_ && panelId == panelId_;
  uint8_t kept = assigned_ ? addr_ : 0;
  havePanel_ = true;
  panelId_ = panelId;
  panelGen_ = panelGen;
  assigned_ = false;
  if (keep) {
    want_ = kept;
  } else {
    want_ = storedPanelId_ == panelId ? storedAddr_ : 0;  // rule 2
  }
  announceDue_ = true;
  announceAt_ = now + rnd(0, 200);
  busErrorsInRow_ = 0;
}

bool ModuleDiscovery::onFrame(const CanFrame& f, uint32_t now) {
  if (!f.extended || f.rtr || f.dlc != 8 || f.id > 0x1FFFFFFF) return false;
  uint8_t func = uint8_t((f.id >> 20) & 0x3F), prio = uint8_t((f.id >> 26) & 7);
  const uint8_t* d = f.data;
  switch (func) {
    case sfunc::kPanelHb:
    case sfunc::kDiscover: {
      AddrId a = AddrId::decode(f.id);
      if (a.src != 0 || a.dst != 0x3F) return false;
      if ((func == sfunc::kPanelHb && prio != 2) || (func == sfunc::kDiscover && prio != 6)) return false;
      onPanel(be32(d), be32(d + 4), now);
      return true;
    }
    case sfunc::kAssign: {
      if (prio != 6 || (f.id & 0xFFFFF) != mac20(d) || memcmp(d, mac_, 6) != 0) return func == sfunc::kAssign;
      if (!havePanel_ || d[7] != uint8_t(panelGen_)) return true;  // rule 1: this panelGen only
      if (d[6] == 0) {                                             // rule 6: RELEASE
        forget(now, false, 0, 200);
        return true;
      }
      if (d[6] > 0x3E) return true;
      if (!assigned_ || addr_ != d[6]) {
        if (assigned_) generation_++;
        addr_ = d[6];
        assigned_ = true;
        want_ = addr_;
        storedAddr_ = addr_;
        storedPanelId_ = panelId_;
        announceDue_ = false;
        beaconAt_ = now + rnd(0, 250);
      }
      return true;
    }
    case sfunc::kBeacon: {
      if (prio != 5 || (f.id & 0xFFFFF) != mac20(d + 1)) return true;
      if (assigned_ && d[0] == addr_ && memcmp(d + 1, mac_, 6) != 0) forget(now, false, 0, 200);  // rule 5
      return true;
    }
    case sfunc::kAnnounce:
      return true;
    default:
      return false;
  }
}

bool ModuleDiscovery::poll(uint32_t now, CanFrame& out) {
  out = CanFrame();
  if (assigned_) {
    if (!expired(beaconAt_, now)) return false;
    out.id = identityId(5, sfunc::kBeacon, mac_);
    out.data[0] = addr_;
    memcpy(out.data + 1, mac_, 6);
    out.data[7] = bootLo_;
    last_ = Last::Beacon;
    return true;
  }
  if (!announceDue_ || !expired(announceAt_, now)) return false;
  out.id = identityId(6, sfunc::kAnnounce, mac_);
  out.data[0] = devType_;
  memcpy(out.data + 1, mac_, 6);
  out.data[7] = want_;
  last_ = Last::Announce;
  return true;
}

void ModuleDiscovery::sent(uint32_t now) {
  if (last_ == Last::Announce) {
    busErrorsInRow_ = 0;
    announceAt_ = now + 1000 + rnd(0, 250);
  } else if (last_ == Last::Beacon) {
    beaconAt_ = now + 1000 - 125 + rnd(0, 250);
  }
  last_ = Last::None;
}

void ModuleDiscovery::lost(uint32_t now) {
  if (last_ == Last::Announce) announceAt_ = now + rnd(1, 20);
  if (last_ == Last::Beacon) beaconAt_ = now + rnd(1, 20);
  last_ = Last::None;
}

void ModuleDiscovery::busError(uint32_t now) {
  if (last_ == Last::Announce) {
    announceAt_ = now + rnd(50, 250);
    if (++busErrorsInRow_ >= 5) {
      announceErrors++;
      busErrorsInRow_ = 0;
    }
  } else if (last_ == Last::Beacon) {
    beaconErrors++;
    beaconAt_ = now + 1000 - 125 + rnd(0, 250);
  }
  last_ = Last::None;
}

}  // namespace zmb
