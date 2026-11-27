#include "zmb.h"

#include <string.h>

#include <atomic>

namespace zmb {

namespace {
uint16_t be16(const uint8_t* b) { return uint16_t(b[0] << 8 | b[1]); }
uint32_t be32(const uint8_t* b) { return uint32_t(b[0]) << 24 | uint32_t(b[1]) << 16 | uint32_t(b[2]) << 8 | b[3]; }
void put16(uint8_t* b, uint16_t v) { b[0] = uint8_t(v >> 8); b[1] = uint8_t(v); }
void put32(uint8_t* b, uint32_t v) { b[0] = uint8_t(v >> 24); b[1] = uint8_t(v >> 16); b[2] = uint8_t(v >> 8); b[3] = uint8_t(v); }
uint8_t prioOf(Kind k) { return (k == Kind::XStart || k == Kind::XData || k == Kind::XAbort) ? 4 : 2; }
uint8_t funcOf(Kind k) {
  switch (k) {
    case Kind::XStart: return func::kXstart;
    case Kind::XData: return func::kXdata;
    case Kind::XAbort: return func::kXabort;
    case Kind::Flow: return func::kFlow;
    case Kind::Ack: return func::kAck;
    case Kind::RAbort: return func::kRabort;
    default: return 0;
  }
}
}  // namespace

uint32_t AddrId::encode() const {
  return (uint32_t(prio & 7) << 26) | (uint32_t(func & 0x3F) << 20) | (uint32_t(src & 0x3F) << 14) | (uint32_t(dst & 0x3F) << 8) | tid8;
}
AddrId AddrId::decode(uint32_t id) {
  return AddrId{uint8_t((id >> 26) & 7), uint8_t((id >> 20) & 0x3F), uint8_t((id >> 14) & 0x3F), uint8_t((id >> 8) & 0x3F), uint8_t(id & 0xFF)};
}

TFrame xstart(uint16_t len, uint16_t tid, uint32_t crc) { TFrame t; t.kind = Kind::XStart; t.len = len; t.tid = tid; t.crc = crc; return t; }
TFrame xdata(uint16_t seq, const uint8_t* six) { TFrame t; t.kind = Kind::XData; t.seq = seq; memcpy(t.bytes, six, 6); return t; }
TFrame flowFrame(uint16_t tid, uint8_t status, uint8_t block, uint8_t stmin, uint16_t expected) {
  TFrame t; t.kind = Kind::Flow; t.tid = tid; t.status = status; t.block = block; t.stmin = stmin; t.expected = expected; return t;
}
TFrame ackFrame(uint16_t tid, uint8_t status, uint32_t crc) { TFrame t; t.kind = Kind::Ack; t.tid = tid; t.status = status; t.crc = crc; return t; }
TFrame abortFrame(Kind k, uint16_t tid, uint8_t r) { TFrame t; t.kind = k; t.tid = tid; t.reason = r; return t; }

CanFrame build(uint8_t src, uint8_t dst, uint8_t tid8, const TFrame& t) {
  CanFrame f;
  f.id = AddrId{prioOf(t.kind), funcOf(t.kind), src, dst, tid8}.encode();
  uint8_t* d = f.data;
  switch (t.kind) {
    case Kind::XStart: put16(d, t.len); put16(d + 2, t.tid); put32(d + 4, t.crc); break;
    case Kind::XData: put16(d, t.seq); memcpy(d + 2, t.bytes, 6); break;
    case Kind::XAbort:
    case Kind::RAbort: put16(d, t.tid); d[2] = t.reason; break;
    case Kind::Flow: put16(d, t.tid); d[2] = t.status; d[3] = t.block; d[4] = t.stmin; put16(d + 5, t.expected); break;
    case Kind::Ack: put16(d, t.tid); d[2] = t.status; put32(d + 3, t.crc); break;
    default: break;
  }
  return f;
}

static bool validReason(uint8_t r) { return r >= reason::kCancel && r <= reason::kSuperseded; }

bool addressed(const CanFrame& f, uint8_t src, uint8_t dst) {
  if (!f.extended || f.rtr || f.id > 0x1FFFFFFFu) return false;
  AddrId a = AddrId::decode(f.id);
  return a.src == src && a.dst == dst;
}

bool RxContext::operator==(const RxContext& o) const {
  return local == o.local && side == o.side && panelGen == o.panelGen && ownBootTag == o.ownBootTag && havePeerBootTag == o.havePeerBootTag &&
         peerBootTag == o.peerBootTag && haveAssignedMac == o.haveAssignedMac && memcmp(assignedMac, o.assignedMac, 6) == 0 && srcAssigned == o.srcAssigned;
}

bool parse(const CanFrame& f, AddrId& a, TFrame& t) {
  if (!f.extended || f.rtr || f.dlc != 8 || f.id > 0x1FFFFFFFu) return false;
  a = AddrId::decode(f.id);
  const uint8_t* d = f.data;
  t = TFrame();
  switch (a.func) {
    case func::kXstart: t.kind = Kind::XStart; t.len = be16(d); t.tid = be16(d + 2); t.crc = be32(d + 4); return true;
    case func::kXdata: t.kind = Kind::XData; t.seq = be16(d); memcpy(t.bytes, d + 2, 6); return true;
    // Unknown codes are dropped here (§1): an unknown ACK status is no proof of anything.
    case func::kXabort: if (!validReason(d[2])) return false; t.kind = Kind::XAbort; t.tid = be16(d); t.reason = d[2]; return true;
    case func::kFlow: if (d[2] > flow::kOverflow) return false; t.kind = Kind::Flow; t.tid = be16(d); t.status = d[2]; t.block = d[3]; t.stmin = d[4]; t.expected = be16(d + 5); return true;
    case func::kAck: if (d[2] > ack::kBadHeader) return false; t.kind = Kind::Ack; t.tid = be16(d); t.status = d[2]; t.crc = be32(d + 3); return true;
    case func::kRabort: if (!validReason(d[2])) return false; t.kind = Kind::RAbort; t.tid = be16(d); t.reason = d[2]; return true;
    default: return false;
  }
}

uint32_t crc32(const uint8_t* data, size_t len) {
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < len; i++) {
    c ^= data[i];
    for (int k = 0; k < 8; k++) c = (c & 1) ? (c >> 1) ^ 0xEDB88320u : c >> 1;
  }
  return ~c;
}

void Header::encode(uint8_t* b) const {
  memset(b, 0, kHeaderLen);
  b[0] = ver; b[1] = mtype;
  put16(b + 4, tid); put16(b + 6, reqTid); put32(b + 8, panelGen); put32(b + 12, bootTag); put32(b + 16, reqCrc);
}
bool Header::decode(const uint8_t* b, size_t len, Header& h) {
  if (len < kHeaderLen) return false;
  h.ver = b[0]; h.mtype = b[1]; h.tid = be16(b + 4); h.reqTid = be16(b + 6);
  h.panelGen = be32(b + 8); h.bootTag = be32(b + 12); h.reqCrc = be32(b + 16);
  return true;
}

size_t Hello::encode(uint8_t* o, size_t cap) const {
  size_t n = 1 + 1 + deviceIdLen + 16 + 1 + firmwareLen + 9;
  if (n > cap) return 0;
  size_t at = 0;
  o[at++] = devType;
  o[at++] = deviceIdLen; memcpy(o + at, deviceId, deviceIdLen); at += deviceIdLen;
  memcpy(o + at, bootId, 16); at += 16;
  o[at++] = firmwareLen; memcpy(o + at, firmware, firmwareLen); at += firmwareLen;
  o[at++] = protoMajor; o[at++] = protoMinor;
  put32(o + at, caps); at += 4;
  put16(o + at, maxMessage); at += 2;
  o[at++] = hwRev;
  return at;
}
bool Hello::decode(const uint8_t* b, size_t len, Hello& h) {
  size_t at = 0;
  if (len < 1) return false;
  h.devType = b[at++];
  if (at >= len) return false;
  h.deviceIdLen = b[at++];
  if (at + h.deviceIdLen > len) return false;
  h.deviceId = reinterpret_cast<const char*>(b + at); at += h.deviceIdLen;
  if (at + 16 > len) return false;
  memcpy(h.bootId, b + at, 16); at += 16;
  if (at >= len) return false;
  h.firmwareLen = b[at++];
  if (at + h.firmwareLen > len) return false;
  h.firmware = reinterpret_cast<const char*>(b + at); at += h.firmwareLen;
  if (at + 9 > len) return false;
  h.protoMajor = b[at]; h.protoMinor = b[at + 1];
  h.caps = be32(b + at + 2); h.maxMessage = be16(b + at + 6); h.hwRev = b[at + 8];
  return true;
}

size_t HelloAck::encode(uint8_t* o) const { o[0] = status; o[1] = addr; put32(o + 2, panelId); o[6] = d10Proto; return 7; }
bool HelloAck::decode(const uint8_t* b, size_t len, HelloAck& h) {
  if (len < 7) return false;
  h.status = b[0]; h.addr = b[1]; h.panelId = be32(b + 2); h.d10Proto = b[6];
  return true;
}

bool deviceIdHasMac(const char* id, size_t len, const uint8_t mac[6]) {
  static const char hex[] = "0123456789ABCDEF";
  char want[12];
  for (int i = 0; i < 6; i++) { want[2 * i] = hex[mac[i] >> 4]; want[2 * i + 1] = hex[mac[i] & 15]; }
  for (size_t s = 0; s + 12 <= len; s++) {
    bool ok = true;
    for (int i = 0; i < 12 && ok; i++) {
      char c = id[s + i];
      if (c >= 'a' && c <= 'f') c = char(c - 32);
      ok = c == want[i];
    }
    if (ok) return true;
  }
  return false;
}

bool expired(uint32_t deadline, uint32_t now) { return int32_t(now - deadline) >= 0; }

// ---- sender ----

bool Sender::start(const uint8_t* msg, size_t len, uint16_t tid) {
  if (state_ != TxState::Idle || pendingAbort_) return false;
  if (len < kMinMessage || len > kMaxMessage) return false;
  msg_ = msg; len_ = len; tid_ = tid;
  crc_ = crc32(msg, len);
  nextSeq_ = 1; lastSeq_ = uint16_t((len + 5) / 6); blockLeft_ = 0; waits_ = 0;
  haveLastXdata_ = false; haveDeadline_ = false;
  state_ = TxState::Starting;
  return true;
}

Sender::Cand Sender::candidate(uint32_t now) const {
  if (pendingAbort_) return Cand::XAbort;
  if (state_ == TxState::Starting) return Cand::XStart;
  if (state_ == TxState::Sending && blockLeft_ > 0) {
    if (!haveLastXdata_ || uint32_t(now - lastXdataAt_) >= stmin_) return Cand::XData;
  }
  return Cand::None;
}

bool Sender::poll(uint32_t now, CanFrame& out) const {
  uint8_t tid8 = uint8_t(tid_ & 0xFF);
  switch (candidate(now)) {
    case Cand::XStart: out = build(local_, peer_, tid8, xstart(uint16_t(len_), tid_, crc_)); return true;
    case Cand::XData: {
      uint8_t six[6] = {0};
      size_t at = size_t(nextSeq_ - 1) * 6;
      size_t n = len_ - at < 6 ? len_ - at : 6;
      memcpy(six, msg_ + at, n);
      out = build(local_, peer_, tid8, xdata(nextSeq_, six));
      return true;
    }
    case Cand::XAbort: out = build(local_, peer_, tid8, abortFrame(Kind::XAbort, tid_, pendingAbort_)); return true;
    default: return false;
  }
}

void Sender::confirm(uint32_t now) {
  switch (candidate(now)) {
    case Cand::XAbort: pendingAbort_ = 0; break;
    case Cand::XStart: state_ = TxState::WaitFlow; deadline_ = now + kNBsMs; haveDeadline_ = true; break;
    case Cand::XData:
      lastXdataAt_ = now; haveLastXdata_ = true;
      blockLeft_--;
      if (nextSeq_ == lastSeq_) {
        state_ = TxState::WaitAck;  // committed (§12)
        deadline_ = now + kTAckMs; haveDeadline_ = true;
      } else if (blockLeft_ == 0) {
        state_ = TxState::WaitFlow;
        deadline_ = now + kNBsMs; haveDeadline_ = true;
      }
      nextSeq_++;
      break;
    default: break;
  }
}

TxOutcome Sender::finish(TxResult r, uint8_t code, uint8_t abortReason) {
  state_ = TxState::Idle;
  haveDeadline_ = false;
  pendingAbort_ = abortReason;
  return TxOutcome{r, code};
}

TxOutcome Sender::onFrame(const CanFrame& f, uint32_t now) {
  AddrId a; TFrame t;
  if (!parse(f, a, t)) {
    if (addressed(f, peer_, local_)) invalid++;  // unknown code: dropped, state unchanged
    return {};
  }
  if (a.src != peer_ || a.dst != local_) return {};
  if (state_ == TxState::Idle || a.tid8 != uint8_t(tid_ & 0xFF)) { invalid++; return {}; }
  if (t.tid != tid_) return {};
  if (t.kind == Kind::Flow && state_ == TxState::WaitFlow) {
    if (t.status == flow::kCts && t.expected == nextSeq_ && t.block > 0) {
      waits_ = 0;
      stmin_ = t.stmin;
      uint16_t left = uint16_t(lastSeq_ - nextSeq_ + 1);
      blockLeft_ = t.block < left ? t.block : left;
      state_ = TxState::Sending;
      haveDeadline_ = false;
      return {};
    }
    if (t.status == flow::kWait) {
      if (++waits_ > kMaxWaits) return finish(TxResult::NotDelivered, reason::kTimeout, reason::kTimeout);
      deadline_ = now + kNBsMs; haveDeadline_ = true;
      return {};
    }
    if (t.status == flow::kOverflow) return finish(TxResult::NotDelivered, reason::kOverflow, 0);
    return {};
  }
  if (t.kind == Kind::RAbort && (state_ == TxState::WaitFlow || state_ == TxState::Sending)) return finish(TxResult::NotDelivered, t.reason, 0);
  if (t.kind == Kind::Ack && state_ == TxState::WaitAck && t.crc == crc_) {
    return t.status == ack::kOk ? finish(TxResult::Delivered, 0, 0) : finish(TxResult::Rejected, t.status, 0);
  }
  return {};  // a late RABORT after the last XDATA proves nothing
}

TxOutcome Sender::tick(uint32_t now) {
  if (!haveDeadline_ || !expired(deadline_, now)) return {};
  if (state_ == TxState::WaitFlow) return finish(TxResult::NotDelivered, reason::kTimeout, reason::kTimeout);
  if (state_ == TxState::WaitAck) return finish(TxResult::Unknown, 0, 0);
  return {};
}

TxOutcome Sender::cancel(uint8_t why) {
  switch (state_) {
    case TxState::Idle: return {};
    case TxState::Starting: return finish(TxResult::NotDelivered, why, 0);
    case TxState::WaitFlow:
    case TxState::Sending: return finish(TxResult::NotDelivered, why, why);
    case TxState::WaitAck: return finish(TxResult::Unknown, 0, 0);
  }
  return {};
}

// ---- receiver ----

CanFrame Receiver::reply(const RxContext& ctx, uint16_t tid, const TFrame& t) const {
  return build(ctx.local, peer_, uint8_t(tid & 0xFF), t);
}

bool Receiver::pendingFlow(CanFrame& out, FlowTicket& ticket) const {
  if (!active_ || !havePending_) return false;
  out = pending_;
  ticket = pendingTicket_;
  return true;
}

bool Receiver::confirmFlow(FlowTicket t, uint32_t now) {
  if (!isCurrent(t)) return false;
  havePending_ = false;
  uint16_t left = uint16_t(last_ - expected_ + 1);
  credit_ = block_ < left ? block_ : left;
  deadline_ = now + kNCrMs;
  return true;
}

RxOutput Receiver::onFrame(const CanFrame& f, const RxContext& ctx, uint32_t now) {
  RxOutput out;
  AddrId a; TFrame t;
  if (!parse(f, a, t)) {
    if (addressed(f, peer_, ctx.local)) invalid++;  // unknown code: dropped, state unchanged
    return out;
  }
  if (a.src != peer_ || a.dst != ctx.local) return out;
  if (t.kind == Kind::XStart) {
    if (a.tid8 != uint8_t(t.tid & 0xFF)) { invalid++; return out; }
    if (!active_ && haveLast_ && !expired(lastAt_ + kDupWindowMs, now) && lastCtx_ == ctx && lastTid_ == t.tid && lastCrc_ == t.crc && lastLen_ == t.len) {
      out.event.kind = RxKind::Duplicate; out.event.tid = t.tid;  // a delayed copy: ignored
      return out;
    }
    haveLast_ = false;
    if (active_) {
      active_ = false;
      out.event.kind = RxKind::Aborted; out.event.tid = tid_; out.event.code = reason::kSuperseded;
    }
    if (t.len > kMaxMessage) {
      out.hasReply = true; out.reply = reply(ctx, t.tid, abortFrame(Kind::RAbort, t.tid, reason::kOverflow));
      return out;
    }
    if (t.len < kMinMessage || t.len > cap_) {
      out.hasReply = true; out.reply = reply(ctx, t.tid, flowFrame(t.tid, flow::kOverflow, 0, 0, 1));
      return out;
    }
    active_ = true; tid_ = t.tid; len_ = t.len; crc_ = t.crc; expected_ = 1; last_ = uint16_t((t.len + 5) / 6); credit_ = 0;
    pending_ = reply(ctx, t.tid, flowFrame(t.tid, flow::kCts, block_, stmin_, 1)); havePending_ = true; pendingTicket_ = newTicket();
    deadline_ = now + kNBsMs;  // our CTS not transmitted yet: the sender gives up after N_Bs
    out.hasReply = true; out.reply = pending_; out.flow = pendingTicket_;
    return out;
  }
  if (t.kind == Kind::XData) {
    if (!active_) return out;
    if (a.tid8 != uint8_t(tid_ & 0xFF)) { invalid++; return out; }
    if (t.seq != 0 && t.seq == uint16_t(expected_ - 1)) return out;  // duplicate
    if (t.seq != expected_ || credit_ == 0) {
      uint8_t why = t.seq != expected_ ? reason::kSeqGap : reason::kOverflow;  // gap, or beyond the granted block
      active_ = false;
      out.hasReply = true; out.reply = reply(ctx, tid_, abortFrame(Kind::RAbort, tid_, why));
      out.event.kind = RxKind::Aborted; out.event.tid = tid_; out.event.code = why;
      return out;
    }
    size_t at = size_t(t.seq - 1) * 6;
    size_t n = len_ - at < 6 ? len_ - at : 6;
    memcpy(buf_ + at, t.bytes, n);
    expected_++; credit_--;
    deadline_ = now + kNCrMs;
    if (t.seq == last_) {
      active_ = false;
      Header h;
      uint8_t status = check(ctx, h);
      out.hasReply = true; out.reply = reply(ctx, tid_, ackFrame(tid_, status, crc_));
      out.event.tid = tid_;
      if (status == ack::kOk) {
        haveLast_ = true; lastTid_ = tid_; lastCrc_ = crc_; lastLen_ = len_; lastAt_ = now; lastCtx_ = ctx;
        out.event.kind = RxKind::Message; out.event.len = len_; out.event.header = h;
      } else {
        out.event.kind = RxKind::Rejected; out.event.code = status;
      }
      return out;
    }
    if (credit_ == 0) {
      pending_ = reply(ctx, tid_, flowFrame(tid_, flow::kCts, block_, stmin_, expected_)); havePending_ = true; pendingTicket_ = newTicket();
      deadline_ = now + kNBsMs;
      out.hasReply = true; out.reply = pending_; out.flow = pendingTicket_;
    }
    return out;
  }
  if (t.kind == Kind::XAbort) {
    if (active_ && t.tid == tid_ && a.tid8 == uint8_t(tid_ & 0xFF)) {
      active_ = false;
      out.event.kind = RxKind::Aborted; out.event.tid = t.tid; out.event.code = t.reason;
    } else {
      invalid++;
    }
  }
  return out;
}

RxEvent Receiver::tick(uint32_t now) {
  RxEvent e;
  if (haveLast_ && expired(lastAt_ + kDupWindowMs, now)) haveLast_ = false;
  if (active_ && expired(deadline_, now)) {
    active_ = false;
    e.kind = RxKind::TimedOut; e.tid = tid_;
  }
  return e;
}

uint8_t Receiver::check(const RxContext& ctx, Header& h) const {
  if (crc32(buf_, len_) != crc_) return ack::kCrcFail;
  if (!Header::decode(buf_, len_, h) || h.ver != 1 || h.tid != tid_) return ack::kBadHeader;
  if (!ctx.srcAssigned) return ack::kGenMismatch;
  MsgType mt = MsgType(h.mtype);
  bool known = h.mtype >= 1 && h.mtype <= 4;
  bool allowed = known && (ctx.side == Side::Panel ? (mt == MsgType::Response || mt == MsgType::Hello)
                                                   : (mt == MsgType::Request || mt == MsgType::HelloAck));
  if (!allowed) return ack::kBadHeader;
  if (h.panelGen != ctx.panelGen) return ack::kGenMismatch;
  bool ok = false;
  switch (mt) {
    case MsgType::Request:
    case MsgType::HelloAck: ok = h.bootTag == ctx.ownBootTag; break;
    case MsgType::Response: ok = ctx.havePeerBootTag && ctx.peerBootTag == h.bootTag; break;
    case MsgType::Hello: {
      Hello hello;
      if (!Hello::decode(buf_ + kHeaderLen, len_ - kHeaderLen, hello)) return ack::kBadHeader;
      ok = be32(hello.bootId) == h.bootTag && ctx.haveAssignedMac && deviceIdHasMac(hello.deviceId, hello.deviceIdLen, ctx.assignedMac);
      break;
    }
  }
  return ok ? ack::kOk : ack::kGenMismatch;
}

bool matchReply(const Header& h, uint16_t tid, uint32_t crc, uint32_t panelGen, uint32_t bootTag) {
  return h.reqTid == tid && h.reqCrc == crc && h.panelGen == panelGen && h.bootTag == bootTag;
}

// ---- TidTable ----

void TidTable::prune(uint32_t now) {
  while (len_ > 0) {
    const TidEntry& e = buf_[head_];
    if (e.active || !expired(e.last + kTidQuarantineMs, now)) break;
    head_ = (head_ + 1) % cap_;
    len_--;
  }
}

static std::atomic<uint32_t> tidOwners{1};

TidTable::TidTable(TidEntry* buf, size_t cap, uint16_t first)
    : buf_(buf), cap_(cap < 65536 ? (cap ? cap : 1) : 65536), nextSerial_(first),
      owner_(tidOwners.fetch_add(1, std::memory_order_relaxed)) {}

bool TidTable::alloc(uint32_t now, TidLease& lease) {
  prune(now);
  if (len_ >= cap_) return false;
  lease.owner = owner_;
  lease.gen = gen_;
  lease.serial = nextSerial_++;
  lease.tid = uint16_t(lease.serial & 0xFFFF);
  TidEntry& e = buf_[(head_ + len_) % cap_];
  e.last = now;
  e.active = 1;
  len_++;
  return true;
}

TidEntry* TidTable::entry(const TidLease& lease) {
  if (lease.owner != owner_ || lease.gen != gen_) return nullptr;
  uint32_t oldest = nextSerial_ - uint32_t(len_);
  uint32_t i = lease.serial - oldest;
  if (i >= len_) return nullptr;
  return &buf_[(head_ + i) % cap_];
}

bool TidTable::touch(const TidLease& lease, uint32_t now) {
  TidEntry* e = entry(lease);
  if (!e) return false;
  e->last = now;
  return true;
}

bool TidTable::finish(const TidLease& lease, uint32_t now) {
  TidEntry* e = entry(lease);
  if (!e) return false;
  e->last = now;
  e->active = 0;
  return true;
}

bool TidTable::freeAt(uint32_t& at) const {
  if (len_ == 0 || buf_[head_].active) return false;
  at = buf_[head_].last + kTidQuarantineMs;
  return true;
}

}  // namespace zmb
