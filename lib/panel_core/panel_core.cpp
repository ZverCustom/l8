#include "panel_core.h"

#include <stdio.h>
#include <string.h>

using namespace zmb;

namespace panel_core {

namespace {
uint32_t be32(const uint8_t* d) {
  return uint32_t(d[0]) << 24 | uint32_t(d[1]) << 16 | uint32_t(d[2]) << 8 | d[3];
}
bool transportFunc(uint8_t fn) {
  return fn == func::kFlow || fn == func::kAck || fn == func::kRabort || fn == func::kXstart || fn == func::kXdata ||
         fn == func::kXabort;
}
}  // namespace

PanelCore::PanelCore(const Identity& id, const Buffers& b, const CoreIo& io, RandFn rand, void* randCtx)
    : id_(id), b_(b), io_(io), disc_(id.mac, id.devType, rand, randCtx), tids_(b.tids, b.tidCap, 1) {
  bootTag_ = be32(id_.bootId);
  disc_.setStored(id_.storedPanelId, id_.storedAddr);
  disc_.setBootLo(uint8_t(bootTag_));
  lastGen_ = disc_.generation();
}

void PanelCore::log(const char* s) const {
  if (io_.log) io_.log(io_.ctx, s);
}

void PanelCore::emit(const CanFrame& f, bool single) {
  if (io_.tx) io_.tx(io_.ctx, f, single);
}

RxContext PanelCore::ctx() const {
  RxContext c;
  c.local = disc_.addr();
  c.side = Side::Module;
  c.panelGen = disc_.panelGen();
  c.ownBootTag = bootTag_;
  return c;
}

void PanelCore::onFrame(const CanFrame& f, const panel_rx::Stamp& s, uint32_t now) {
  if (!f.extended) {
    c_.drop_std++;
    return;
  }
  if (f.rtr) {
    c_.drop_rtr++;
    return;
  }
  uint8_t fn = uint8_t((f.id >> 20) & 0x3F);
  if (fn == 0x01 || fn == 0x02 || fn == 0x04 || fn == 0x05) {
    panel_rx::Frame pf;
    pf.id = f.id;
    pf.ext = true;
    pf.dlc = f.dlc;
    memcpy(pf.data, f.data, 8);
    c_.panel++;
    bool hb = rxPanel_.onFrame(pf, s) != panel_rx::Result::Bad && fn == 0x04;
    if (hb && disc_.onFrame(f, now)) pump(now);
    return;
  }
  if (f.dlc != 8) {
    c_.drop_dlc++;
    return;
  }
  if (disc_.onFrame(f, now)) {
    c_.discovery++;
    pump(now);
    return;
  }
  if (!transportFunc(fn) || f.id > 0x1FFFFFFF) {
    c_.drop_not_ours++;
    return;
  }
  AddrId a = AddrId::decode(f.id);
  if (!disc_.assigned()) {
    c_.drop_unassigned++;
    return;
  }
  if (a.dst != disc_.addr()) {
    c_.drop_other_dst++;
    return;
  }
  if (a.src != 0x00) {
    c_.drop_other_src++;
    return;
  }
  c_.transport++;
  transport(f, now);
}

void PanelCore::transport(const CanFrame& f, uint32_t now) {
  if (!haveTransport_) return;
  outcome(tx_.onFrame(f, now), now);
  RxOutput r = rx_.onFrame(f, ctx(), now);
  if (r.hasReply) {
    emit(r.reply, false);
    if (r.flow) rx_.confirmFlow(r.flow, now);
  }
  if (r.event.kind == RxKind::Message) onMessage(r.event, now);
  pump(now);
}

void PanelCore::onMessage(const RxEvent& e, uint32_t now) {
  const uint8_t* m = rx_.message();
  if (e.header.mtype == uint8_t(MsgType::HelloAck)) {
    HelloAck a;
    if (phase_ == Phase::Hello && matchReply(e.header, helloTid_, helloCrc_, disc_.panelGen(), bootTag_) &&
        HelloAck::decode(m + kHeaderLen, e.len - kHeaderLen, a) && a.addr == disc_.addr()) {
      phase_ = Phase::Ready;  // any status, as zmb_node: an unsupported type stays present, D10 sends it nothing
      statusAt_ = now;
      char t[24];
      snprintf(t, sizeof t, "hello_ack %u", unsigned(a.status));
      log(t);
      snprintf(t, sizeof t, "ready %u", unsigned(disc_.addr()));
      log(t);
    }
    return;
  }
  if (e.header.mtype != uint8_t(MsgType::Request)) return;
  c_.requests++;
  if (phase_ != Phase::Ready) {
    c_.not_ready++;
    return;
  }
  uint32_t crc = crc32(m, e.len);
  if (haveLastReq_ && lastReqGen_ == disc_.panelGen() && lastReqTid_ == e.header.tid && lastReqCrc_ == crc &&
      int32_t(now - lastReqAt_) < int32_t(kTidQuarantineMs)) {
    c_.dup_request++;
    log("dup_request");
    return;
  }
  if (respPending_ || respInFlight_) {
    c_.busy_dropped++;
    log("busy_dropped");
    return;
  }
  haveLastReq_ = true;
  lastReqGen_ = disc_.panelGen();
  lastReqTid_ = e.header.tid;
  lastReqCrc_ = crc;
  lastReqAt_ = now;
  c_.dispatched++;
  size_t n = 0;
  if (io_.dispatch && b_.respCap > kHeaderLen)
    n = io_.dispatch(io_.ctx, m + kHeaderLen, e.len - kHeaderLen, b_.resp + kHeaderLen, b_.respCap - kHeaderLen);
  if (n == 0 || n > b_.respCap - kHeaderLen) return;
  Header h;
  h.mtype = uint8_t(MsgType::Response);
  h.reqTid = e.header.tid;
  h.panelGen = disc_.panelGen();
  h.bootTag = bootTag_;
  h.reqCrc = crc;
  h.encode(b_.resp);  // tid filled at start
  respLen_ = kHeaderLen + n;
  respPending_ = true;
}

bool PanelCore::startMsg(MsgType t, uint8_t* msg, size_t len, uint32_t now) {
  if (!haveTransport_ || tx_.state() != TxState::Idle) return false;
  TidLease l;
  if (!tids_.alloc(now, l)) {
    log("tidwait");
    return false;
  }
  // the tid goes into the header (bytes 4..5, §5.1)
  msg[4] = uint8_t(l.tid >> 8);
  msg[5] = uint8_t(l.tid);
  if (!tx_.start(msg, len, l.tid)) {
    tids_.finish(l, now);
    return false;
  }
  lease_ = l;
  haveLease_ = true;
  if (t == MsgType::Hello) {
    helloTid_ = l.tid;
    helloCrc_ = crc32(msg, len);
    helloAt_ = now;
  }
  return true;
}

void PanelCore::sendHello(uint32_t now) {
  Hello hl;
  hl.devType = id_.devType;
  hl.deviceId = id_.deviceId;
  hl.deviceIdLen = uint8_t(id_.deviceId ? strlen(id_.deviceId) : 0);
  memcpy(hl.bootId, id_.bootId, 16);
  hl.firmware = id_.firmware;
  hl.firmwareLen = uint8_t(id_.firmware ? strlen(id_.firmware) : 0);
  hl.caps = 0x1F;
  hl.maxMessage = 24576;
  hl.hwRev = id_.hwRev;
  Header h;
  h.mtype = uint8_t(MsgType::Hello);
  h.panelGen = disc_.panelGen();
  h.bootTag = bootTag_;
  h.encode(hello_);
  size_t n = hl.encode(hello_ + kHeaderLen, sizeof hello_ - kHeaderLen);
  if (n && startMsg(MsgType::Hello, hello_, kHeaderLen + n, now)) phase_ = Phase::Hello;
}

void PanelCore::boundary(uint32_t now) {
  if (haveLease_) tids_.finish(lease_, now);
  haveLease_ = false;
  haveTransport_ = false;
  respPending_ = respInFlight_ = false;
  phase_ = Phase::Unassigned;
  c_.boundaries++;
  log("boundary");
}

void PanelCore::pump(uint32_t now) {
  if (disc_.generation() != lastGen_) {  // address lost or changed: transport state ends
    lastGen_ = disc_.generation();
    boundary(now);
  }
  if (!haveTidGen_ || tidGen_ != disc_.panelGen()) {  // a new panelGen: a new tid space
    if (haveTidGen_ && haveTransport_) boundary(now);
    tids_.reset(1);
    tidGen_ = disc_.panelGen();
    haveTidGen_ = true;
    haveLease_ = false;
  }
  if (disc_.assigned() && !haveTransport_) {
    tx_ = Sender(disc_.addr(), 0);
    rx_ = Receiver(0, b_.rx, b_.rxCap);
    haveTransport_ = true;
    sendHello(now);
  }
  if (phase_ == Phase::Hello && int32_t(now - helloAt_) > 2000) {  // no HELLO_ACK: start over
    c_.hello_timeouts++;
    log("hello_timeout");
    disc_.restart(now);
    pump(now);
    return;
  }
  if (respPending_ && haveTransport_ && tx_.state() == TxState::Idle) {
    if (startMsg(MsgType::Response, b_.resp, respLen_, now)) {
      respPending_ = false;
      respInFlight_ = true;
    }
  }
  CanFrame f;
  while (haveTransport_ && tx_.poll(now, f)) {
    emit(f, false);
    tx_.confirm(now);
    if (haveLease_) tids_.touch(lease_, now);
  }
}

void PanelCore::outcome(const TxOutcome& o, uint32_t now) {
  if (o.result == TxResult::None) return;
  if (haveLease_) tids_.finish(lease_, now);
  haveLease_ = false;
  if (respInFlight_) {
    respInFlight_ = false;
    if (o.result == TxResult::Delivered) c_.responses_sent++;
  }
  log(o.result == TxResult::Delivered ? "tx delivered" : o.result == TxResult::Unknown ? "tx unknown" : "tx failed");
}

void PanelCore::tick(uint32_t now) {
  rxPanel_.expire(now);
  if (haveTransport_) {
    outcome(tx_.tick(now), now);
    RxEvent e = rx_.tick(now);
    if (e.kind != RxKind::None) log("rx_end");
  }
  pump(now);
  if (phase_ == Phase::Ready && expired(statusAt_, now)) {
    statusAt_ = now + 500;
    uint8_t flags = 0, chOn = 0, chFault = 0;
    uint16_t rev = 0, saved = 0xFFFF;
    if (io_.statusA) io_.statusA(io_.ctx, &flags, &rev, &saved, &chOn, &chFault);
    CanFrame s;
    AddrId a{5, sfunc::kStatusA, disc_.addr(), 0x3F, 0};
    s.id = a.encode();
    s.data[0] = flags;
    s.data[1] = statusSeq_++;
    s.data[2] = uint8_t(rev >> 8);
    s.data[3] = uint8_t(rev);
    s.data[4] = uint8_t(saved >> 8);
    s.data[5] = uint8_t(saved);
    s.data[6] = chOn;
    s.data[7] = chFault;
    emit(s, false);
  }
  CanFrame single;
  if (disc_.poll(now, single)) emit(single, true);
}

void PanelCore::singleShotResult(SingleShot r, uint32_t now) {
  if (r == SingleShot::Sent) disc_.sent(now);
  else if (r == SingleShot::Lost) disc_.lost(now);
  else disc_.busError(now);
  pump(now);
}

VehicleView PanelCore::panelVehicle(uint32_t now) const {
  using namespace panel_rx;
  VehicleView v;
  const FieldVal& rpm = rxPanel_.field(kRpm);
  if (rpm.has) {
    v.rpm = rpm.value;
    v.lastUpdate = rpm.m ? rpm.m : rpm.m - 1;  // 0 means "no data" to the existing rule; older, not younger
    if (rxPanel_.fresh(kRpm, now)) v.fresh |= kFreshRpm;
  }
  if (rxPanel_.fresh(kSpeed, now)) {
    uint16_t s = rxPanel_.field(kSpeed).value / 10;
    v.speed = uint8_t(s > 255 ? 255 : s);
    v.fresh |= kFreshSpeed;
  }
  if (rxPanel_.fresh(kThrottle, now)) {
    v.throttle = uint8_t(rxPanel_.field(kThrottle).value);
    v.fresh |= kFreshThrottle;
  }
  if (rxPanel_.fresh(kCoolant, now)) {
    v.temp = int8_t(int(rxPanel_.field(kCoolant).value) - 40);
    v.fresh |= kFreshTemp;
  }
  return v;
}

}  // namespace panel_core
