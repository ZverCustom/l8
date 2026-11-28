#include "panel_rx.h"

namespace panel_rx {

namespace {

int32_t diff(uint32_t a, uint32_t b) { return static_cast<int32_t>(a - b); }

uint32_t be32(const uint8_t* d) {
  return static_cast<uint32_t>(d[0]) << 24 | static_cast<uint32_t>(d[1]) << 16 | static_cast<uint32_t>(d[2]) << 8 | d[3];
}

uint8_t funcOf(uint32_t id) { return (id >> 20) & 0x3F; }

}  // namespace

Kind classify(const Frame& f, BadWhy* why) {
  *why = BadWhy::None;
  if (!f.ext) {
    *why = BadWhy::StdId;
    return Kind::Bad;
  }
  uint32_t id = f.id & 0x1FFFFFFF;
  uint8_t fn = funcOf(id);
  uint32_t want;
  Kind k;
  switch (fn) {
    case 0x01: want = kIdVehFast; k = Kind::VehFast; break;
    case 0x02: want = kIdVehSlow; k = Kind::VehSlow; break;
    case 0x04: want = kIdPanelHb; k = Kind::PanelHb; break;
    case 0x05: want = kIdPanelState; k = Kind::PanelState; break;
    default: return Kind::NotOurs;
  }
  if (f.id != want) {
    *why = BadWhy::Header;
    return Kind::Bad;
  }
  if (f.dlc != 8) {
    *why = BadWhy::Dlc;
    return Kind::Bad;
  }
  return k;
}

VehFast parseVehFast(const uint8_t d[8]) {
  VehFast v;
  v.rpm = static_cast<uint16_t>(d[0] << 8 | d[1]);
  v.speed = static_cast<uint16_t>(d[2] << 8 | d[3]);
  v.throttle = d[4];
  v.valid = d[5] & 0x07;
  v.age = d[6];
  v.seq = d[7];
  return v;
}

VehSlow parseVehSlow(const uint8_t d[8]) {
  VehSlow v;
  v.coolant = d[0];
  v.oil = d[1];
  v.gear = d[2];
  v.ign = d[3];
  v.valid = d[4] & 0x0F;
  v.age = d[5];
  v.seq = d[6];
  return v;
}

PanelHb parsePanelHb(const uint8_t d[8]) { return PanelHb{be32(d), be32(d + 4)}; }

PanelState parsePanelState(const uint8_t d[8]) {
  PanelState s;
  s.mcuGen = be32(d);
  s.flags = d[4] & 0x07;
  s.seq = d[5];
  s.vehAgeMax = d[6];
  return s;
}

Poll QueueStamp::poll(const Snapshot& s) {
  if (s.overrun) {
    a_ = Anchor{};
  } else if (s.empty) {
    a_.has = true;  // C++11 (the firmware's gnu++11)
    a_.t = s.t0;
  }
  Poll p;
  p.a_ = a_;
  return p;
}

Stamp QueueStamp::read(const Poll& p, uint32_t tRead, bool overrunGrew) {
  Anchor a = p.a_;
  if (overrunGrew) {
    a_ = Anchor{};
    a = Anchor{};
  }
  Stamp s;
  s.tRead = tRead;
  if (a.has && diff(tRead, a.t) <= kMaxQueueMs && diff(tRead, a.t) >= 0) {
    s.usable = true;
    s.anchor = a.t;
  }
  return s;
}

bool Consumer::seqAccept(Seq& q, uint8_t seq, uint32_t t) {
  if (q.has && diff(t, q.at) > kSeqSilenceMs) {
    q.has = false;
    c_.seq_reset_silence++;
  }
  if (q.has) {
    uint8_t d = static_cast<uint8_t>(seq - q.last);
    if (d < 1 || d > 127) return false;
  }
  q.has = true;
  q.last = seq;
  q.at = t;
  return true;
}

void Consumer::applyFields(uint8_t valid, uint8_t age, const Stamp& s, Field first, int n, const uint16_t* v) {
  if (!valid) return;
  if (age == 0xFF) {
    c_.age_missing++;
    return;
  }
  if (age == 0xFE) {
    c_.age_unbounded++;
    return;
  }
  if (!s.usable) {
    c_.stamp_unusable++;
    return;
  }
  uint32_t m = s.anchor - static_cast<uint32_t>(age) * 4 - kMarginMs;
  for (int i = 0; i < n; i++) {
    if (!(valid >> i & 1)) continue;
    if (first >= kCoolant && v[i] == 0xFF) {
      c_.value_invalid++;
      continue;
    }
    FieldVal& f = f_[first + i];
    f.has = true;
    f.value = v[i];
    f.m = m;
    c_.fields_updated++;
  }
}

Result Consumer::onFrame(const Frame& f, const Stamp& s) {
  expire(s.tRead);
  BadWhy why;
  Kind k = classify(f, &why);
  switch (k) {
    case Kind::NotOurs:
      c_.not_ours++;
      return Result::NotOurs;
    case Kind::Bad:
      if (why == BadWhy::StdId) c_.bad_std_id++;
      else if (why == BadWhy::Dlc) c_.bad_dlc++;
      else c_.bad_header++;
      return Result::Bad;
    case Kind::VehFast: {
      VehFast v = parseVehFast(f.data);
      if (!seqAccept(fast_, v.seq, s.tRead)) {
        c_.seq_dropped++;
        return Result::SeqDropped;
      }
      c_.veh_fast++;
      uint16_t vals[3] = {v.rpm, v.speed, v.throttle};
      applyFields(v.valid, v.age, s, kRpm, 3, vals);
      return Result::Accepted;
    }
    case Kind::VehSlow: {
      VehSlow v = parseVehSlow(f.data);
      if (!seqAccept(slow_, v.seq, s.tRead)) {
        c_.seq_dropped++;
        return Result::SeqDropped;
      }
      c_.veh_slow++;
      uint16_t vals[4] = {v.coolant, v.oil, v.gear, v.ign};
      applyFields(v.valid, v.age, s, kCoolant, 4, vals);
      return Result::Accepted;
    }
    case Kind::PanelHb: {
      PanelHb h = parsePanelHb(f.data);
      if (hasHb_ && h.panelGen != hb_.panelGen) {
        fast_.has = slow_.has = false;
        c_.seq_reset_gen++;
      }
      hb_ = h;
      hasHb_ = true;
      hbAt_ = s.tRead;
      c_.panel_hb++;
      return Result::Accepted;
    }
    case Kind::PanelState: {
      PanelState st = parsePanelState(f.data);
      if (hasState_ && st.mcuGen != state_.mcuGen) {
        fast_.has = slow_.has = false;
        c_.seq_reset_gen++;
      }
      state_ = st;
      hasState_ = true;
      stateAt_ = s.tRead;
      c_.panel_state++;
      return Result::Accepted;
    }
  }
  return Result::Bad;
}

void Consumer::expire(uint32_t now) {
  for (FieldVal& f : f_) {
    if (f.has && diff(now, f.m) > kForgetMs) f.has = false;
  }
}

int32_t Consumer::ageMs(Field i, uint32_t now) const {
  if (!f_[i].has) return -1;
  return diff(now, f_[i].m);
}

bool Consumer::fresh(Field i, uint32_t now) const {
  int32_t a = ageMs(i, now);
  return a >= 0 && a <= kFreshMs;
}

bool Consumer::vehicleDataFresh(uint32_t now) const {
  int32_t a = ageMs(kRpm, now);
  return a >= 0 && a <= kRunMs;
}

bool Consumer::engineRunning(uint32_t now) const { return vehicleDataFresh(now) && f_[kRpm].value > kRunRpm; }

}  // namespace panel_rx
