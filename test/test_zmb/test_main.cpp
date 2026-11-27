// ZMB core (lib/zmb) against the shared corpus test/test_zmb/corpus_v1.txt and the
// §5.4-§5.6 traces. Native only; the firmware CAN path does not use it yet.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unity.h>

#include <map>
#include <string>
#include <vector>

#include "zmb.h"
#include "zmb_disc.h"

using namespace zmb;

static std::multimap<std::string, std::vector<std::string>> corpus;

static void loadCorpus() {
  const char* path = getenv("ZMB_CORPUS");
  FILE* f = fopen(path ? path : "test/test_zmb/corpus_v1.txt", "r");
  TEST_ASSERT_NOT_NULL_MESSAGE(f, "corpus file");
  char line[8192];
  while (fgets(line, sizeof line, f)) {
    if (line[0] == '#' || line[0] == '\n') continue;
    std::vector<std::string> parts;
    for (char* tok = strtok(line, " \r\n"); tok; tok = strtok(nullptr, " \r\n")) parts.emplace_back(tok);
    if (parts.empty()) continue;
    std::string key = parts[0];
    parts.erase(parts.begin());
    corpus.emplace(key, parts);
  }
  fclose(f);
}
static std::vector<std::string> get(const char* k) {
  auto it = corpus.find(k);
  TEST_ASSERT_TRUE_MESSAGE(it != corpus.end(), k);
  return it->second;
}
static std::vector<uint8_t> hex(const std::string& s) {
  std::vector<uint8_t> o;
  for (size_t i = 0; i + 1 < s.size(); i += 2) o.push_back(uint8_t(strtoul(s.substr(i, 2).c_str(), nullptr, 16)));
  return o;
}
static uint32_t hx(const std::string& s) { return uint32_t(strtoul(s.c_str(), nullptr, 16)); }

static const uint32_t kGen = 0x9F2C1A07, kBoot = 0x01234567;
static const uint8_t kMac[6] = {0x10, 0xBD, 0xA3, 0x5F, 0x51, 0x14};
static uint8_t rxbuf[kMaxMessage];

static RxContext panelCtx() {
  RxContext c; c.local = 0; c.side = Side::Panel; c.panelGen = kGen; c.havePeerBootTag = true; c.peerBootTag = kBoot;
  c.haveAssignedMac = true; memcpy(c.assignedMac, kMac, 6); return c;
}
static RxContext moduleCtx() { RxContext c; c.local = 1; c.side = Side::Module; c.panelGen = kGen; c.ownBootTag = kBoot; return c; }

static std::vector<uint8_t> message(MsgType t, uint16_t tid, uint16_t reqTid, uint32_t reqCrc, const std::vector<uint8_t>& payload) {
  Header h; h.mtype = uint8_t(t); h.tid = tid; h.reqTid = reqTid; h.panelGen = kGen; h.bootTag = kBoot; h.reqCrc = reqCrc;
  std::vector<uint8_t> m(kHeaderLen);
  h.encode(m.data());
  m.insert(m.end(), payload.begin(), payload.end());
  return m;
}

// One sender, one receiver and the bus; `mutate` may change or drop (return false) a frame.
struct Link {
  Sender tx;
  Receiver rx;
  RxContext ctx;
  uint32_t now = 1000;
  std::vector<CanFrame> bus;
  TxOutcome out;
  std::vector<RxEvent> events;
  Link(uint8_t txLocal, uint8_t rxLocal, RxContext c) : tx(txLocal, rxLocal), rx(txLocal, rxbuf, sizeof rxbuf), ctx(c) {}
  template <class F>
  void run(F mutate) {
    CanFrame f;
    for (int i = 0; i < 20000 && tx.poll(now, f); i++) {
      tx.confirm(now);
      bus.push_back(f);
      if (!mutate(f)) continue;
      RxOutput o = rx.onFrame(f, ctx, now);
      if (o.event.kind != RxKind::None) events.push_back(o.event);
      if (o.hasReply) {
        if (o.flow) rx.confirmFlow(o.flow, now);  // the harness is the lower layer: on the bus = transmitted
        bus.push_back(o.reply);
        TxOutcome r = tx.onFrame(o.reply, now);
        if (r.result != TxResult::None) out = r;
      }
    }
  }
  void run() { run([](CanFrame&) { return true; }); }
  int messages() const { int n = 0; for (auto& e : events) n += e.kind == RxKind::Message; return n; }
};

static std::vector<uint8_t> traceReq() { return message(MsgType::Request, 0x0102, 0, 0, hex(get("trace.request.json")[0])); }
static std::vector<uint8_t> traceResp(const std::vector<uint8_t>& req) {
  return message(MsgType::Response, 0x0057, 0x0102, crc32(req.data(), req.size()), hex(get("trace.response.json")[0]));
}

static void test_crc32_known_vector() {
  auto v = get("crc32.check");
  auto data = hex(v[0]);
  TEST_ASSERT_EQUAL_HEX32(hx(v[1]), crc32(data.data(), data.size()));
  TEST_ASSERT_EQUAL_HEX32(0xCBF43926, crc32(reinterpret_cast<const uint8_t*>("123456789"), 9));
}

static void test_hello_vector_byte_exact_with_every_frame() {
  std::string id = get("hello.device_id")[0], fw = get("hello.firmware")[0];
  Hello h;
  h.deviceId = id.c_str(); h.deviceIdLen = uint8_t(id.size());
  auto boot = hex(get("hello.boot_id")[0]); memcpy(h.bootId, boot.data(), 16);
  h.firmware = fw.c_str(); h.firmwareLen = uint8_t(fw.size());
  h.caps = hx(get("hello.caps")[0]); h.maxMessage = uint16_t(hx(get("hello.max_message")[0])); h.hwRev = uint8_t(hx(get("hello.hw_rev")[0]));
  uint8_t payload[128];
  size_t n = h.encode(payload, sizeof payload);
  auto msg = message(MsgType::Hello, 1, 0, 0, std::vector<uint8_t>(payload, payload + n));
  auto want = hex(get("hello.message")[0]);
  TEST_ASSERT_EQUAL(76, msg.size());
  TEST_ASSERT_EQUAL_HEX8_ARRAY(want.data(), msg.data(), 76);
  TEST_ASSERT_EQUAL_HEX32(0x69075B7D, crc32(msg.data(), msg.size()));
  Link l(1, 0, panelCtx());
  TEST_ASSERT_TRUE(l.tx.start(msg.data(), msg.size(), 1));
  l.run();
  std::vector<std::pair<uint32_t, std::vector<uint8_t>>> frames;
  for (auto r = corpus.equal_range("hello.frame"); r.first != r.second; ++r.first) frames.push_back({hx(r.first->second[0]), hex(r.first->second[1])});
  TEST_ASSERT_EQUAL(frames.size(), l.bus.size());
  for (size_t i = 0; i < frames.size(); i++) {
    TEST_ASSERT_EQUAL_HEX32(frames[i].first, l.bus[i].id);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(frames[i].second.data(), l.bus[i].data, 8);
  }
  TEST_ASSERT_EQUAL(int(TxResult::Delivered), int(l.out.result));
  TEST_ASSERT_EQUAL(1, l.messages());
}

static void test_trace_79_frames_and_reply_matches() {
  auto req = traceReq();
  auto resp = traceResp(req);
  TEST_ASSERT_EQUAL(180, req.size());
  TEST_ASSERT_EQUAL(236, resp.size());
  Link a(0, 1, moduleCtx());
  a.tx.start(req.data(), req.size(), 0x0102);
  a.run();
  TEST_ASSERT_EQUAL(int(TxResult::Delivered), int(a.out.result));
  Link b(1, 0, panelCtx());
  b.tx.start(resp.data(), resp.size(), 0x0057);
  b.run();
  TEST_ASSERT_EQUAL(int(TxResult::Delivered), int(b.out.result));
  TEST_ASSERT_EQUAL(79, a.bus.size() + b.bus.size());
  const Header& h = b.events.back().header;
  TEST_ASSERT_TRUE(matchReply(h, 0x0102, crc32(req.data(), req.size()), kGen, kBoot));
  TEST_ASSERT_FALSE(matchReply(h, 0x0002, crc32(req.data(), req.size()), kGen, kBoot));
  TEST_ASSERT_FALSE(matchReply(h, 0x0102, 1, kGen, kBoot));
  TEST_ASSERT_FALSE(matchReply(h, 0x0102, crc32(req.data(), req.size()), 1, kBoot));
  TEST_ASSERT_FALSE(matchReply(h, 0x0102, crc32(req.data(), req.size()), kGen, 1));
}

static void test_seq_gap_duplicate_corruption() {
  auto req = traceReq();
  {
    Link l(0, 1, moduleCtx());
    l.tx.start(req.data(), req.size(), 0x0102);
    l.run([](CanFrame& f) { AddrId a; TFrame t; return !(parse(f, a, t) && t.kind == Kind::XData && t.seq == 5); });
    TEST_ASSERT_EQUAL(int(TxResult::NotDelivered), int(l.out.result));
    TEST_ASSERT_EQUAL(reason::kSeqGap, l.out.code);
    TEST_ASSERT_EQUAL(0, l.messages());
  }
  {
    Link l(0, 1, moduleCtx());
    l.tx.start(req.data(), req.size(), 0x0102);
    bool silent = false;
    CanFrame f;
    for (int i = 0; i < 200 && l.tx.poll(l.now, f); i++) {
      l.tx.confirm(l.now);
      for (int rep = 0; rep < 2; rep++) {
        RxOutput o = l.rx.onFrame(f, l.ctx, l.now);
        AddrId a; TFrame t; parse(f, a, t);
        if (rep == 1) silent = silent || (t.kind == Kind::XData && t.seq == 3 && !o.hasReply && o.event.kind == RxKind::None);
        if (o.hasReply) { if (o.flow) l.rx.confirmFlow(o.flow, l.now); TxOutcome r = l.tx.onFrame(o.reply, l.now); if (r.result != TxResult::None) l.out = r; }
        if (!(t.kind == Kind::XData && t.seq == 3)) break;
      }
    }
    TEST_ASSERT_TRUE(silent);
    TEST_ASSERT_EQUAL(int(TxResult::Delivered), int(l.out.result));
  }
  {
    Link l(0, 1, moduleCtx());
    l.tx.start(req.data(), req.size(), 0x0102);
    l.run([](CanFrame& f) { AddrId a; TFrame t; if (parse(f, a, t) && t.kind == Kind::XData && t.seq == 7) f.data[4] ^= 1; return true; });
    TEST_ASSERT_EQUAL(int(TxResult::Rejected), int(l.out.result));
    TEST_ASSERT_EQUAL(ack::kCrcFail, l.out.code);
    TEST_ASSERT_EQUAL(0, l.messages());
  }
}

static TxOutcome rejectWith(RxContext ctx, const std::vector<uint8_t>& msg, uint16_t tid, uint8_t txLocal) {
  Link l(txLocal, ctx.local, ctx);
  l.tx.start(msg.data(), msg.size(), tid);
  l.run();
  TEST_ASSERT_EQUAL(0, l.messages());
  return l.out;
}

static void test_generations_types_and_hello_identity() {
  auto req = traceReq(), resp = traceResp(req);
  RxContext c = moduleCtx(); c.panelGen = 1;
  TEST_ASSERT_EQUAL(ack::kGenMismatch, rejectWith(c, req, 0x0102, 0).code);
  c = moduleCtx(); c.ownBootTag = 9;
  TEST_ASSERT_EQUAL(ack::kGenMismatch, rejectWith(c, req, 0x0102, 0).code);
  c = moduleCtx(); c.srcAssigned = false;
  TEST_ASSERT_EQUAL(ack::kGenMismatch, rejectWith(c, req, 0x0102, 0).code);
  RxContext p = panelCtx(); p.peerBootTag = 9;
  TEST_ASSERT_EQUAL(ack::kGenMismatch, rejectWith(p, resp, 0x0057, 1).code);
  TEST_ASSERT_EQUAL(ack::kBadHeader, rejectWith(panelCtx(), req, 0x0102, 1).code);  // REQUEST to the panel
  TEST_ASSERT_EQUAL(ack::kBadHeader, rejectWith(moduleCtx(), req, 0x0103, 0).code);  // header tid ≠ XSTART tid
  auto hello = hex(get("hello.message")[0]);
  p = panelCtx(); uint8_t other[6] = {1, 2, 3, 4, 5, 6}; memcpy(p.assignedMac, other, 6);
  TEST_ASSERT_EQUAL(ack::kGenMismatch, rejectWith(p, hello, 1, 1).code);
  auto bad = hello; bad[12] ^= 0xFF;
  TEST_ASSERT_EQUAL(ack::kGenMismatch, rejectWith(panelCtx(), bad, 1, 1).code);
}

static void test_length_limits_and_non_zmb_frames() {
  auto small = message(MsgType::Request, 7, 0, 0, {});
  Link a(0, 1, moduleCtx());
  a.tx.start(small.data(), small.size(), 7);
  a.run();
  TEST_ASSERT_EQUAL(int(TxResult::Delivered), int(a.out.result));
  static std::vector<uint8_t> big;
  big = message(MsgType::Request, 8, 0, 0, std::vector<uint8_t>(kMaxMessage - kHeaderLen, 'a'));
  Link b(0, 1, moduleCtx());
  b.tx.start(big.data(), big.size(), 8);
  b.run();
  TEST_ASSERT_EQUAL(int(TxResult::Delivered), int(b.out.result));
  TEST_ASSERT_EQUAL(kMaxMessage, b.events.back().len);
  Sender s(0, 1);
  TEST_ASSERT_FALSE(s.start(big.data(), 19, 1));
  TEST_ASSERT_FALSE(s.start(big.data(), kMaxMessage + 1, 1));
  Receiver r(0, rxbuf, sizeof rxbuf);
  RxOutput o = r.onFrame(build(0, 1, 1, xstart(24597, 1, 0)), moduleCtx(), 0);
  AddrId ad; TFrame t;
  TEST_ASSERT_TRUE(parse(o.reply, ad, t));
  TEST_ASSERT_EQUAL(int(Kind::RAbort), int(t.kind));
  TEST_ASSERT_EQUAL(reason::kOverflow, t.reason);
  o = r.onFrame(build(0, 1, 2, xstart(19, 2, 0)), moduleCtx(), 0);
  parse(o.reply, ad, t);
  TEST_ASSERT_EQUAL(int(Kind::Flow), int(t.kind));
  TEST_ASSERT_EQUAL(flow::kOverflow, t.status);
  uint8_t tinyBuf[64];
  Receiver tiny(0, tinyBuf, sizeof tinyBuf);  // a caller buffer smaller than the message
  o = tiny.onFrame(build(0, 1, 3, xstart(180, 3, 0)), moduleCtx(), 0);
  parse(o.reply, ad, t);
  TEST_ASSERT_EQUAL(flow::kOverflow, t.status);
  CanFrame ok = build(0, 1, 3, xstart(30, 3, 0));
  CanFrame x = ok; x.extended = false; TEST_ASSERT_FALSE(parse(x, ad, t));
  x = ok; x.rtr = true; TEST_ASSERT_FALSE(parse(x, ad, t));
  x = ok; x.dlc = 7; TEST_ASSERT_FALSE(parse(x, ad, t));
  x = ok; x.id = 0x20000000; TEST_ASSERT_FALSE(parse(x, ad, t));
}

static void test_waits_stmin_block() {
  auto req = traceReq();
  Sender s(0, 1);
  s.start(req.data(), req.size(), 0x0102);
  s.confirm(0);
  CanFrame wait = build(1, 0, 0x02, flowFrame(0x0102, flow::kWait, 0, 0, 1));
  for (int i = 0; i < 3; i++) TEST_ASSERT_EQUAL(int(TxResult::None), int(s.onFrame(wait, 1).result));
  TxOutcome o = s.onFrame(wait, 1);
  TEST_ASSERT_EQUAL(int(TxResult::NotDelivered), int(o.result));
  TEST_ASSERT_EQUAL(reason::kTimeout, o.code);
  CanFrame f; AddrId a; TFrame t;
  TEST_ASSERT_TRUE(s.poll(1, f));
  parse(f, a, t);
  TEST_ASSERT_EQUAL(int(Kind::XAbort), int(t.kind));
  Sender s2(0, 1);
  s2.start(req.data(), req.size(), 0x0102);
  s2.confirm(0);
  s2.onFrame(build(1, 0, 0x02, flowFrame(0x0102, flow::kCts, 2, 5, 1)), 0);
  TEST_ASSERT_TRUE(s2.poll(0, f));
  s2.confirm(0);
  TEST_ASSERT_FALSE(s2.poll(4, f));
  TEST_ASSERT_TRUE(s2.poll(5, f));
  s2.confirm(5);
  TEST_ASSERT_FALSE(s2.poll(50, f));
  TEST_ASSERT_EQUAL(int(TxState::WaitFlow), int(s2.state()));
  // a FLOW of another tid is ignored and gives no XDATA
  s2.onFrame(build(1, 0, 0x02, flowFrame(0x0002, flow::kCts, 16, 0, 3)), 50);
  TEST_ASSERT_FALSE(s2.poll(50, f));
}

static void test_timers_cancel_generation_wrap() {
  auto req = traceReq();
  Sender s(0, 1);
  s.start(req.data(), req.size(), 1);
  s.confirm(0xFFFFFFFFu - 10);
  TEST_ASSERT_EQUAL(int(TxResult::None), int(s.tick(0xFFFFFFFFu).result));
  TEST_ASSERT_EQUAL(int(TxResult::NotDelivered), int(s.tick(uint32_t(0xFFFFFFFFu - 10 + kNBsMs)).result));
  Receiver r(0, rxbuf, sizeof rxbuf);
  r.onFrame(build(0, 1, 1, xstart(180, 1, 0)), moduleCtx(), 0xFFFFFFFFu - 50);
  // our CTS not transmitted yet: the hold limit is N_Bs
  TEST_ASSERT_EQUAL(int(RxKind::None), int(r.tick(0xFFFFFFFFu).kind));
  TEST_ASSERT_EQUAL(int(RxKind::None), int(r.tick(49).kind));
  TEST_ASSERT_EQUAL(int(RxKind::TimedOut), int(r.tick(99).kind));
  // after the CTS is transmitted the pause between XDATA is N_Cr
  RxOutput c0 = r.onFrame(build(0, 1, 1, xstart(180, 1, 0)), moduleCtx(), 0xFFFFFFFFu - 50);
  TEST_ASSERT_TRUE(r.confirmFlow(c0.flow, 0xFFFFFFFFu - 50));
  TEST_ASSERT_EQUAL(int(RxKind::None), int(r.tick(0xFFFFFFFFu).kind));
  TEST_ASSERT_EQUAL(int(RxKind::TimedOut), int(r.tick(49).kind));
  auto small = message(MsgType::Request, 2, 0, 0, {'{', '}'});
  CanFrame f;
  Sender c(0, 1);
  c.start(small.data(), small.size(), 2);
  c.confirm(0);
  c.onFrame(build(1, 0, 2, flowFrame(2, flow::kCts, 16, 0, 1)), 0);
  TEST_ASSERT_TRUE(c.poll(0, f));  // the last XDATA offered, not written yet
  TEST_ASSERT_EQUAL(int(TxState::Sending), int(c.state()));
  Sender c2 = c;
  TxOutcome o = c2.cancel(reason::kCancel);
  TEST_ASSERT_EQUAL(int(TxResult::NotDelivered), int(o.result));
  while (c.poll(1, f)) c.confirm(1);
  TEST_ASSERT_TRUE(c.committed());
  Sender c3 = c;
  TEST_ASSERT_EQUAL(int(TxResult::Unknown), int(c3.generationChanged().result));
  TEST_ASSERT_EQUAL(int(TxResult::None), int(c.onFrame(build(1, 0, 2, abortFrame(Kind::RAbort, 2, reason::kSeqGap)), 2).result));
  TEST_ASSERT_EQUAL(int(TxResult::None), int(c.onFrame(build(1, 0, 2, ackFrame(2, ack::kOk, 0)), 2).result));
  TEST_ASSERT_EQUAL(int(TxResult::Unknown), int(c.tick(1 + kTAckMs).result));
}

static void test_duplicate_xstart_and_supersede() {
  auto small = message(MsgType::Request, 9, 0, 0, {'{', '}'});
  Link l(0, 1, moduleCtx());
  l.tx.start(small.data(), small.size(), 9);
  l.run();
  RxOutput o = l.rx.onFrame(l.bus[0], l.ctx, l.now);
  TEST_ASSERT_EQUAL(int(RxKind::Duplicate), int(o.event.kind));
  TEST_ASSERT_FALSE(o.hasReply);  // a delayed copy: no ACK, no FLOW
  TEST_ASSERT_FALSE(l.rx.busy());
  TEST_ASSERT_EQUAL(1, l.messages());
  Receiver r(0, rxbuf, sizeof rxbuf);
  r.onFrame(build(0, 1, 2, xstart(180, 0x0102, 0)), moduleCtx(), 0);
  o = r.onFrame(build(0, 1, 2, xstart(30, 0x0002, 0)), moduleCtx(), 1);
  TEST_ASSERT_EQUAL(int(RxKind::Aborted), int(o.event.kind));
  TEST_ASSERT_EQUAL(reason::kSuperseded, o.event.code);
  o = r.onFrame(build(0, 1, 2, abortFrame(Kind::XAbort, 0x0002, reason::kCancel)), moduleCtx(), 2);
  TEST_ASSERT_EQUAL(int(RxKind::Aborted), int(o.event.kind));
  TEST_ASSERT_FALSE(r.busy());
}

static void test_reserved_written_zero_and_ignored() {
  Header h; h.mtype = 1; h.tid = 1; h.panelGen = 2; h.bootTag = 3;
  uint8_t b[20]; h.encode(b);
  TEST_ASSERT_EQUAL(0, b[2]); TEST_ASSERT_EQUAL(0, b[3]);
  b[2] = b[3] = 0xFF;
  Header g; TEST_ASSERT_TRUE(Header::decode(b, 20, g));
  TEST_ASSERT_EQUAL(1, g.tid);
  CanFrame f = build(1, 0, 1, flowFrame(1, 0, 16, 0, 1));
  TEST_ASSERT_EQUAL(0, f.data[7]);
}

// ---- R44: credit is granted only by a transmitted CTS ----

static std::vector<CanFrame> framesOf(const std::vector<uint8_t>& m, uint16_t tid) {
  std::vector<CanFrame> fs;
  uint8_t t8 = uint8_t(tid & 0xFF);
  fs.push_back(build(0, 1, t8, xstart(uint16_t(m.size()), tid, crc32(m.data(), m.size()))));
  for (size_t at = 0, seq = 1; at < m.size(); at += 6, seq++) {
    uint8_t b[6] = {0};
    memcpy(b, m.data() + at, m.size() - at < 6 ? m.size() - at : 6);
    fs.push_back(build(0, 1, t8, xdata(uint16_t(seq), b)));
  }
  return fs;
}

static bool isMessage(const RxOutput& o) { return o.event.kind == RxKind::Message; }

static void test_r44_credit_only_from_transmitted_cts() {
  auto m = message(MsgType::Request, 7, 0, 0, std::vector<uint8_t>(100, ' '));  // 120 B = 20 XDATA
  auto f = framesOf(m, 7);
  RxContext ctx = moduleCtx();
  {  // initial CTS formed but not transmitted
    Receiver rx(0, rxbuf, sizeof rxbuf);
    RxOutput o = rx.onFrame(f[0], ctx, 0);
    CanFrame p; FlowTicket pt; TEST_ASSERT_TRUE(rx.pendingFlow(p, pt)); TEST_ASSERT_EQUAL(o.flow, pt);
    o = rx.onFrame(f[1], ctx, 0);
    TEST_ASSERT_EQUAL(int(RxKind::Aborted), int(o.event.kind));
    TEST_ASSERT_EQUAL(reason::kOverflow, o.event.code);
    AddrId a; TFrame t; TEST_ASSERT_TRUE(parse(o.reply, a, t));
    TEST_ASSERT_EQUAL(int(Kind::RAbort), int(t.kind));
    TEST_ASSERT_FALSE(rx.busy());
  }
  {  // next CTS held: XDATA 17.. refused, nothing delivered; asking again grants nothing
    Receiver rx(0, rxbuf, sizeof rxbuf);
    RxOutput o = rx.onFrame(f[0], ctx, 0);
    TEST_ASSERT_TRUE(rx.confirmFlow(o.flow, 0));
    CanFrame next; bool formed = false;
    for (int i = 1; i <= 16; i++) { o = rx.onFrame(f[i], ctx, 0); if (o.hasReply) { next = o.reply; formed = true; } }
    TEST_ASSERT_TRUE(formed);
    for (int k = 0; k < 3; k++) { CanFrame p; FlowTicket pt; TEST_ASSERT_TRUE(rx.pendingFlow(p, pt)); }
    o = rx.onFrame(f[17], ctx, 0);
    TEST_ASSERT_EQUAL(int(RxKind::Aborted), int(o.event.kind));
    TEST_ASSERT_EQUAL(reason::kOverflow, o.event.code);
    bool delivered = false;
    for (size_t i = 18; i < f.size(); i++) delivered = delivered || isMessage(rx.onFrame(f[i], ctx, 0));
    TEST_ASSERT_FALSE(delivered);
  }
  {  // refused, then transmitted: completes; an old CTS grants nothing
    Receiver rx(0, rxbuf, sizeof rxbuf);
    rx.onFrame(f[0], ctx, 0);
    CanFrame ff; FlowTicket first, second;
    TEST_ASSERT_TRUE(rx.pendingFlow(ff, first));
    TEST_ASSERT_TRUE(rx.confirmFlow(first, 5));
    for (int i = 1; i <= 16; i++) rx.onFrame(f[i], ctx, 5);
    TEST_ASSERT_TRUE(rx.pendingFlow(ff, second));
    TEST_ASSERT_FALSE(rx.confirmFlow(first, 6));
    TEST_ASSERT_TRUE(rx.confirmFlow(second, 30));
    TEST_ASSERT_FALSE(rx.confirmFlow(second, 31));  // the same confirmation twice
    bool delivered = false;
    for (size_t i = 17; i < f.size(); i++) delivered = delivered || isMessage(rx.onFrame(f[i], ctx, 30));
    TEST_ASSERT_TRUE(delivered);
  }
  {  // stale CTS after supersede or timeout
    Receiver rx(0, rxbuf, sizeof rxbuf);
    FlowTicket old = rx.onFrame(build(0, 1, 1, xstart(60, 1, 0)), ctx, 0).flow;
    rx.onFrame(build(0, 1, 2, xstart(60, 2, 0)), ctx, 1);
    TEST_ASSERT_FALSE(rx.confirmFlow(old, 2));
    CanFrame cf; FlowTicket cur; TEST_ASSERT_TRUE(rx.pendingFlow(cf, cur));
    rx.tick(1 + kNBsMs);
    TEST_ASSERT_FALSE(rx.busy());
    TEST_ASSERT_FALSE(rx.confirmFlow(cur, 200));
  }
}

static void test_r44_full_size_with_deferred_grants() {
  auto big = message(MsgType::Request, 0x0400, 0, 0, std::vector<uint8_t>(kMaxMessage - 20, 'q'));
  Sender tx(0, 1);
  Receiver rx(0, rxbuf, sizeof rxbuf);
  RxContext ctx = moduleCtx();
  TEST_ASSERT_TRUE(tx.start(big.data(), big.size(), 0x0400));
  uint32_t now = 0; bool held = false; CanFrame h; FlowTicket ht = 0; TxOutcome out; bool msg = false;
  for (int i = 0; i < 20000; i++) {
    now++;
    CanFrame f;
    if (tx.poll(now, f)) {
      tx.confirm(now);
      RxOutput o = rx.onFrame(f, ctx, now);
      if (isMessage(o)) msg = o.event.len == kMaxMessage;
      if (o.hasReply) {
        AddrId a; TFrame t; parse(o.reply, a, t);
        if (t.kind == Kind::Flow && t.status == flow::kCts) { held = true; h = o.reply; ht = o.flow; }
        else { TxOutcome r = tx.onFrame(o.reply, now); if (r.result != TxResult::None) out = r; }
      }
    } else if (held) {
      held = false;
      TEST_ASSERT_TRUE(rx.isCurrent(ht));
      TEST_ASSERT_TRUE(rx.confirmFlow(ht, now));
      tx.onFrame(h, now);
    } else {
      break;
    }
  }
  TEST_ASSERT_EQUAL(int(TxResult::Delivered), int(out.result));
  TEST_ASSERT_TRUE(msg);
}

// ---- R45: unknown codes and a wrong tid8 change nothing ----

static CanFrame rawCode(uint8_t fn, uint8_t code, uint32_t crc) {
  CanFrame f = build(1, 0, 2, ackFrame(2, 0, crc));
  AddrId a; a.prio = 2; a.func = fn; a.src = 1; a.dst = 0; a.tid8 = 2;
  f.id = a.encode();
  f.data[2] = code;
  return f;
}

static void test_r45_unknown_codes_and_tid8() {
  auto small = message(MsgType::Request, 2, 0, 0, {'{', '}'});
  Sender s(0, 1);
  s.start(small.data(), small.size(), 2);
  CanFrame f; s.poll(0, f); s.confirm(0);
  TEST_ASSERT_EQUAL(int(TxResult::None), int(s.onFrame(rawCode(func::kFlow, 3, 0), 0).result));
  TEST_ASSERT_EQUAL(int(TxState::WaitFlow), int(s.state()));
  s.onFrame(build(1, 0, 2, flowFrame(2, flow::kCts, 16, 0, 1)), 0);
  while (s.poll(0, f)) s.confirm(0);
  TEST_ASSERT_TRUE(s.committed());
  uint32_t before = s.invalid;
  TEST_ASSERT_EQUAL(int(TxResult::None), int(s.onFrame(rawCode(func::kAck, 6, s.crc()), 1).result));
  TEST_ASSERT_EQUAL(int(TxResult::None), int(s.onFrame(rawCode(func::kAck, 254, s.crc()), 1).result));
  TEST_ASSERT_EQUAL(int(TxResult::None), int(s.onFrame(rawCode(func::kRabort, 0, 0), 1).result));
  TEST_ASSERT_EQUAL(before + 3, s.invalid);
  TEST_ASSERT_TRUE(s.committed());
  TEST_ASSERT_EQUAL(int(TxResult::Unknown), int(s.tick(kTAckMs).result));
  // a valid ACK != 0 is still a refusal
  Sender s2(0, 1);
  s2.start(small.data(), small.size(), 2);
  s2.poll(0, f); s2.confirm(0);
  s2.onFrame(build(1, 0, 2, flowFrame(2, flow::kCts, 16, 0, 1)), 0);
  while (s2.poll(0, f)) s2.confirm(0);
  TxOutcome o = s2.onFrame(build(1, 0, 2, ackFrame(2, ack::kGenMismatch, s2.crc())), 1);
  TEST_ASSERT_EQUAL(int(TxResult::Rejected), int(o.result));
  TEST_ASSERT_EQUAL(ack::kGenMismatch, o.code);
  // receiver: unknown XABORT reason, XSTART / XABORT with another tid8
  RxContext ctx = moduleCtx();
  Receiver rx(0, rxbuf, sizeof rxbuf);
  CanFrame bad = build(0, 1, 7, xstart(60, 7, 0));
  bad.id ^= 1;
  RxOutput r = rx.onFrame(bad, ctx, 0);
  TEST_ASSERT_FALSE(r.hasReply); TEST_ASSERT_EQUAL(int(RxKind::None), int(r.event.kind)); TEST_ASSERT_FALSE(rx.busy());
  RxOutput live = rx.onFrame(build(0, 1, 8, xstart(60, 8, 0)), ctx, 0);
  r = rx.onFrame(bad, ctx, 1);
  TEST_ASSERT_EQUAL(int(RxKind::None), int(r.event.kind));
  CanFrame p; FlowTicket pt; TEST_ASSERT_TRUE(rx.pendingFlow(p, pt));
  TEST_ASSERT_EQUAL_MEMORY(live.reply.data, p.data, 8);
  TEST_ASSERT_EQUAL(live.flow, pt);
  CanFrame ab = build(0, 1, 8, abortFrame(Kind::XAbort, 8, reason::kCancel));
  ab.id ^= 3;
  TEST_ASSERT_EQUAL(int(RxKind::None), int(rx.onFrame(ab, ctx, 2).event.kind));
  CanFrame unk = build(0, 1, 8, abortFrame(Kind::XAbort, 8, reason::kCancel));
  unk.data[2] = 9;
  TEST_ASSERT_EQUAL(int(RxKind::None), int(rx.onFrame(unk, ctx, 2).event.kind));
  TEST_ASSERT_TRUE(rx.busy());
  TEST_ASSERT_EQUAL(4, rx.invalid);
}

// ---- R46: the delivered-message memory has a context and an end ----

static void test_r46_cache_context_and_expiry() {
  auto small = message(MsgType::Request, 9, 0, 0, {'{', '}'});
  {
    Link l(0, 1, moduleCtx());
    l.tx.start(small.data(), small.size(), 9);
    l.run();
    RxContext c = moduleCtx(); c.panelGen += 1; c.srcAssigned = false;
    RxOutput o = l.rx.onFrame(l.bus[0], c, l.now + 10);
    TEST_ASSERT_NOT_EQUAL(int(RxKind::Duplicate), int(o.event.kind));
    AddrId a; TFrame t;
    TEST_ASSERT_FALSE(o.hasReply && parse(o.reply, a, t) && t.kind == Kind::Ack && t.status == ack::kOk);
  }
  {
    Link l(0, 1, moduleCtx());
    l.tx.start(small.data(), small.size(), 9);
    l.run();
    TEST_ASSERT_EQUAL(int(TxResult::Delivered), int(l.out.result));
    l.now += 61000;
    l.rx.tick(l.now);
    l.out = TxOutcome();
    l.events.clear();
    TEST_ASSERT_TRUE(l.tx.start(small.data(), small.size(), 9));
    l.run();
    TEST_ASSERT_EQUAL(int(TxResult::Delivered), int(l.out.result));
    TEST_ASSERT_EQUAL(1, l.messages());
  }
}

// ---- R44 (U3-004): a CTS is identified by its local ticket, never by its bytes ----

static RxOutput start7(Receiver& rx, const RxContext& c, uint16_t len, uint32_t crc, uint32_t now) {
  return rx.onFrame(build(0, 1, 7, xstart(len, 7, crc)), c, now);
}

static bool sameBytes(const CanFrame& a, const CanFrame& b) { return a.id == b.id && memcmp(a.data, b.data, 8) == 0; }

// feeds XDATA [from, to) of m with tid 7; returns the last event kind / code seen
static RxEvent feed(Receiver& rx, const RxContext& c, const std::vector<uint8_t>& m, size_t from, size_t to, uint32_t now) {
  RxEvent last;
  for (size_t k = from; k < to && k * 6 < m.size(); k++) {
    uint8_t b[6] = {0};
    memcpy(b, m.data() + k * 6, m.size() - k * 6 < 6 ? m.size() - k * 6 : 6);
    RxOutput o = rx.onFrame(build(0, 1, 7, xdata(uint16_t(k + 1), b)), c, now);
    if (o.event.kind != RxKind::None) last = o.event;
  }
  return last;
}

static void test_r44_ticket_identity() {
  RxContext ctx = moduleCtx();
  {  // same-tid supersede: identical bytes, the old ticket does not confirm the new one
    auto m = message(MsgType::Request, 7, 0, 0, std::vector<uint8_t>(40, ' '));
    Receiver rx(0, rxbuf, sizeof rxbuf);
    RxOutput a = start7(rx, ctx, uint16_t(m.size()), 123, 0);
    RxOutput b = start7(rx, ctx, uint16_t(m.size()), crc32(m.data(), m.size()), 1);
    TEST_ASSERT_TRUE(sameBytes(a.reply, b.reply));
    TEST_ASSERT_NOT_EQUAL(a.flow, b.flow);
    TEST_ASSERT_FALSE(rx.isCurrent(a.flow));
    TEST_ASSERT_FALSE(rx.confirmFlow(a.flow, 2));
    CanFrame pf; FlowTicket pt; TEST_ASSERT_TRUE(rx.pendingFlow(pf, pt));
    TEST_ASSERT_EQUAL(b.flow, pt);  // the current candidate survives
    TEST_ASSERT_EQUAL(int(RxKind::None), int(rx.tick(1 + kNBsMs - 1).kind));  // its timer did not move
    TEST_ASSERT_TRUE(rx.busy());
    RxEvent e = feed(rx, ctx, m, 0, 1, 3);
    TEST_ASSERT_EQUAL(int(RxKind::Aborted), int(e.kind));
    TEST_ASSERT_EQUAL(reason::kOverflow, e.code);
  }
  {  // new panelGen: identical bytes, no delivery without its own confirmation
    RxContext c = ctx;
    Receiver rx(0, rxbuf, sizeof rxbuf);
    RxOutput a = start7(rx, c, 20, 123, 0);
    c.panelGen++;
    auto m = message(MsgType::Request, 7, 0, 0, {});
    Header h; h.mtype = uint8_t(MsgType::Request); h.tid = 7; h.panelGen = c.panelGen; h.bootTag = c.ownBootTag; h.encode(m.data());
    RxOutput b = start7(rx, c, 20, crc32(m.data(), 20), 1);
    TEST_ASSERT_TRUE(sameBytes(a.reply, b.reply));
    TEST_ASSERT_FALSE(rx.confirmFlow(a.flow, 2));
    TEST_ASSERT_NOT_EQUAL(int(RxKind::Message), int(feed(rx, c, m, 0, 4, 3).kind));
    RxOutput again = start7(rx, c, 20, crc32(m.data(), 20), 4);
    TEST_ASSERT_TRUE(rx.confirmFlow(again.flow, 4));
    TEST_ASSERT_EQUAL(int(RxKind::Message), int(feed(rx, c, m, 0, 4, 5).kind));
  }
  {  // timed out, then a new assembly with the same bytes
    Receiver rx(0, rxbuf, sizeof rxbuf);
    RxOutput a = start7(rx, ctx, 60, 1, 0);
    TEST_ASSERT_EQUAL(int(RxKind::TimedOut), int(rx.tick(kNBsMs).kind));
    RxOutput b = start7(rx, ctx, 60, 1, 200);
    TEST_ASSERT_TRUE(sameBytes(a.reply, b.reply));
    TEST_ASSERT_FALSE(rx.confirmFlow(a.flow, 201));
    TEST_ASSERT_TRUE(rx.isCurrent(b.flow));
  }
  {  // the current ticket grants exactly one block, once
    auto m = message(MsgType::Request, 7, 0, 0, std::vector<uint8_t>(100, ' '));
    Receiver rx(0, rxbuf, sizeof rxbuf);
    RxOutput a = start7(rx, ctx, uint16_t(m.size()), crc32(m.data(), m.size()), 0);
    TEST_ASSERT_TRUE(rx.confirmFlow(a.flow, 0));
    TEST_ASSERT_FALSE(rx.confirmFlow(a.flow, 0));
    TEST_ASSERT_EQUAL(int(RxKind::None), int(feed(rx, ctx, m, 0, 16, 1).kind));
    CanFrame pf; FlowTicket t2; TEST_ASSERT_TRUE(rx.pendingFlow(pf, t2));
    TEST_ASSERT_NOT_EQUAL(a.flow, t2);
    TEST_ASSERT_FALSE(rx.confirmFlow(a.flow, 2));
    RxEvent e = feed(rx, ctx, m, 16, 17, 2);
    TEST_ASSERT_EQUAL(int(RxKind::Aborted), int(e.kind));
    TEST_ASSERT_EQUAL(reason::kOverflow, e.code);
  }
}

// ---- tid table (§5.5) ----

static TidEntry tidAll[65536];

static void test_tid_all_taken_waits_and_quarantine_from_last_frame() {
  TidTable t(tidAll, 65536, 0);
  TidLease first, s;
  TEST_ASSERT_TRUE(t.alloc(0, first));
  TEST_ASSERT_EQUAL(0, first.tid);
  for (uint32_t i = 1; i < 65536; i++) {
    TEST_ASSERT_TRUE(t.alloc(i / 1000, s));
    t.finish(s, i / 1000);
  }
  TEST_ASSERT_FALSE(t.alloc(100, s));
  TEST_ASSERT_FALSE(t.alloc(65000, s));  // the oldest is active: no age-out from its XSTART
  uint32_t at;
  TEST_ASSERT_FALSE(t.freeAt(at));
  TEST_ASSERT_TRUE(t.touch(first, 70000));
  TEST_ASSERT_TRUE(t.finish(first, 80000));
  TEST_ASSERT_TRUE(t.freeAt(at));
  TEST_ASSERT_EQUAL_UINT32(80000 + kTidQuarantineMs, at);
  TEST_ASSERT_FALSE(t.alloc(80000 + kTidQuarantineMs - 1, s));
  TEST_ASSERT_TRUE(t.alloc(80000 + kTidQuarantineMs, s));
  TEST_ASSERT_EQUAL(0, s.tid);
}

static void test_tid_wrap_and_stale_serials() {
  TidEntry b[1];
  TidTable t(b, 1, 65535);
  TidLease a, c;
  uint32_t start = 0xFFFFFFFFu - 100;
  TEST_ASSERT_TRUE(t.alloc(start, a));
  TEST_ASSERT_EQUAL(65535, a.tid);
  t.finish(a, start);
  TEST_ASSERT_FALSE(t.alloc(start + 59999, c));
  TEST_ASSERT_TRUE(t.alloc(start + 60000, c));
  TEST_ASSERT_EQUAL(0, c.tid);  // u16 wraps, u32 clock wrapped too
  TEST_ASSERT_FALSE(t.finish(a, start));  // the old entry left: its lease changes nothing
  TEST_ASSERT_TRUE(t.touch(c, 5));
  // a new panelGen: a new counter space
  t.reset(0x0057);
  TEST_ASSERT_FALSE(t.touch(c, 6));
  TEST_ASSERT_TRUE(t.alloc(7, c));
  TEST_ASSERT_EQUAL(0x0057, c.tid);
}

// R49: after reset (same serial again) and after re-creating the table over the same
// buffer, old leases change nothing; current ones work; the active entry stays taken.
static void test_r49_old_lease_after_reset_or_new_owner() {
  TidEntry b[1];
  TidTable t(b, 1);
  TidLease old, cur;
  TEST_ASSERT_TRUE(t.alloc(0, old));
  t.reset();
  TEST_ASSERT_TRUE(t.alloc(1, cur));
  TEST_ASSERT_EQUAL_UINT32(old.serial, cur.serial);  // same serial, other space
  TEST_ASSERT_FALSE(t.finish(old, 2));
  TEST_ASSERT_FALSE(t.touch(old, 2));
  uint32_t at;
  TEST_ASSERT_FALSE(t.freeAt(at));  // the new entry is still active
  TidLease x;
  TEST_ASSERT_FALSE(t.alloc(70000, x));  // and keeps its tid taken
  TEST_ASSERT_TRUE(t.touch(cur, 3));
  TEST_ASSERT_TRUE(t.finish(cur, 4));
  TEST_ASSERT_TRUE(t.freeAt(at));
  TEST_ASSERT_EQUAL_UINT32(4 + kTidQuarantineMs, at);
  // a new owner over the same buffer: leases of the previous table are foreign
  TidTable u(b, 1);
  TidLease mine;
  TEST_ASSERT_TRUE(u.alloc(5, mine));
  TEST_ASSERT_EQUAL_UINT32(cur.serial, mine.serial);
  TEST_ASSERT_FALSE(u.finish(cur, 6));
  TEST_ASSERT_FALSE(u.freeAt(at));
  TEST_ASSERT_TRUE(u.finish(mine, 7));
}


// ---- module discovery (§4.2) ----

static uint32_t fixedRand(void* ctx) { return *static_cast<uint32_t*>(ctx); }

static CanFrame panelFrame(uint8_t func, uint8_t prio, uint32_t panelId, uint32_t gen) {
  CanFrame f;
  AddrId a{prio, func, 0, 0x3F, 0};
  f.id = a.encode();
  uint8_t d[8] = {uint8_t(panelId >> 24), uint8_t(panelId >> 16), uint8_t(panelId >> 8), uint8_t(panelId),
                  uint8_t(gen >> 24), uint8_t(gen >> 16), uint8_t(gen >> 8), uint8_t(gen)};
  memcpy(f.data, d, 8);
  return f;
}
static CanFrame assignFrame(const uint8_t mac[6], uint8_t addr, uint8_t genLo) {
  CanFrame f;
  f.id = identityId(6, sfunc::kAssign, mac);
  memcpy(f.data, mac, 6);
  f.data[6] = addr;
  f.data[7] = genLo;
  return f;
}
static CanFrame beaconFrame(const uint8_t mac[6], uint8_t addr) {
  CanFrame f;
  f.id = identityId(5, sfunc::kBeacon, mac);
  f.data[0] = addr;
  memcpy(f.data + 1, mac, 6);
  return f;
}

// Trace A (§4.4): both start at 37, A (lower ID) wins, B lost → retried 1..20 ms later;
// ASSIGN only with our MAC and this panelGen; then BEACON every 1000 ± 125.
static void test_disc_trace_a_and_assign_rules() {
  const uint8_t macA[6] = {0x10, 0xBD, 0xA3, 0x5F, 0x51, 0x14}, macB[6] = {0x10, 0xBD, 0xA3, 0x5F, 0x88, 0x02};
  uint32_t r = 37;
  ModuleDiscovery a(macA, 1, fixedRand, &r), b(macB, 1, fixedRand, &r);
  CanFrame hb = panelFrame(sfunc::kPanelHb, 2, 0x11223344, 0x9F2C1A07), out;
  TEST_ASSERT_TRUE(a.onFrame(hb, 0));
  b.onFrame(hb, 0);
  TEST_ASSERT_FALSE(a.poll(36, out));
  TEST_ASSERT_TRUE(a.poll(37, out));
  TEST_ASSERT_EQUAL_HEX32(0x1BEF5114, out.id);
  TEST_ASSERT_EQUAL_UINT8(0, out.data[7]);
  a.sent(37);
  TEST_ASSERT_TRUE(b.poll(37, out));
  TEST_ASSERT_EQUAL_HEX32(0x1BEF8802, out.id);
  b.lost(37);  // arbitration lost, nothing sent
  TEST_ASSERT_FALSE(b.poll(37 + 17, out));
  TEST_ASSERT_TRUE(b.poll(37 + 18, out));  // 1 + 37 % 20
  b.sent(55);
  // ASSIGN for another MAC or another panelGen changes nothing
  a.onFrame(assignFrame(macB, 1, 0x07), 60);
  a.onFrame(assignFrame(macA, 1, 0x08), 60);
  TEST_ASSERT_FALSE(a.assigned());
  a.onFrame(assignFrame(macA, 1, 0x07), 87);
  b.onFrame(assignFrame(macB, 2, 0x07), 95);
  TEST_ASSERT_TRUE(a.assigned() && b.assigned());
  TEST_ASSERT_EQUAL_UINT8(1, a.addr());
  TEST_ASSERT_EQUAL_UINT8(2, b.addr());
  TEST_ASSERT_TRUE(a.poll(87 + 37, out));  // first BEACON 0..250 ms after ASSIGN
  TEST_ASSERT_EQUAL_HEX32(identityId(5, sfunc::kBeacon, macA), out.id);
  a.sent(124);
  TEST_ASSERT_FALSE(a.poll(124 + 911, out));
  TEST_ASSERT_TRUE(a.poll(124 + 912, out));  // 875 + 37
}

// Rules 2, 5, 6, 7 and the announce retry/bus-error counters.
static void test_disc_want_conflict_release_and_new_gen() {
  const uint8_t mac[6] = {0x10, 0xBD, 0xA3, 0x5F, 0x51, 0x14}, other[6] = {0x10, 0xBD, 0xA3, 0x5F, 0x88, 0x02};
  uint32_t r = 0;
  ModuleDiscovery d(mac, 1, fixedRand, &r);
  d.setStored(0x11223344, 5);
  CanFrame out;
  d.onFrame(panelFrame(sfunc::kPanelHb, 2, 0x11223344, 0x01), 0);
  TEST_ASSERT_TRUE(d.poll(0, out));
  TEST_ASSERT_EQUAL_UINT8(5, out.data[7]);  // rule 2: same panelId → stored address wanted
  d.sent(0);
  TEST_ASSERT_FALSE(d.poll(999, out));
  TEST_ASSERT_TRUE(d.poll(1000, out));  // sent without ASSIGN → 1000 + 0..250
  for (int i = 0; i < 5; i++) {
    TEST_ASSERT_TRUE(d.poll(2000 + i * 100, out));
    d.busError(2000 + i * 100);
  }
  TEST_ASSERT_EQUAL_UINT32(1, d.announceErrors);
  d.onFrame(assignFrame(mac, 5, 0x01), 3000);
  uint32_t g = d.generation();
  TEST_ASSERT_TRUE(d.assigned());
  // rule 7: new panelGen, same panel → confirmation forgotten, address kept as wantAddr
  d.onFrame(panelFrame(sfunc::kDiscover, 6, 0x11223344, 0x02), 3100);
  TEST_ASSERT_FALSE(d.assigned());
  TEST_ASSERT_TRUE(d.generation() > g);
  TEST_ASSERT_TRUE(d.poll(3100, out));
  TEST_ASSERT_EQUAL_UINT8(5, out.data[7]);
  d.sent(3100);
  d.onFrame(assignFrame(mac, 5, 0x02), 3110);
  TEST_ASSERT_TRUE(d.assigned());
  // rule 5: another MAC beacons our address → back to ANNOUNCE with wantAddr 0
  g = d.generation();
  d.onFrame(beaconFrame(other, 5), 3200);
  TEST_ASSERT_FALSE(d.assigned());
  TEST_ASSERT_TRUE(d.generation() > g);
  TEST_ASSERT_TRUE(d.poll(3200, out));
  TEST_ASSERT_EQUAL_UINT8(0, out.data[7]);
  d.sent(3200);
  d.onFrame(assignFrame(mac, 6, 0x02), 3210);
  TEST_ASSERT_EQUAL_UINT8(6, d.addr());
  // rule 6: RELEASE
  d.onFrame(assignFrame(mac, 0, 0x02), 3300);
  TEST_ASSERT_FALSE(d.assigned());
  TEST_ASSERT_EQUAL_UINT8(0, d.addr());
  TEST_ASSERT_TRUE(d.poll(3300, out));
  TEST_ASSERT_EQUAL_UINT8(0, out.data[7]);
}

int main(int, char**) {
  UNITY_BEGIN();
  loadCorpus();
  RUN_TEST(test_crc32_known_vector);
  RUN_TEST(test_hello_vector_byte_exact_with_every_frame);
  RUN_TEST(test_trace_79_frames_and_reply_matches);
  RUN_TEST(test_seq_gap_duplicate_corruption);
  RUN_TEST(test_generations_types_and_hello_identity);
  RUN_TEST(test_length_limits_and_non_zmb_frames);
  RUN_TEST(test_waits_stmin_block);
  RUN_TEST(test_timers_cancel_generation_wrap);
  RUN_TEST(test_duplicate_xstart_and_supersede);
  RUN_TEST(test_reserved_written_zero_and_ignored);
  RUN_TEST(test_r44_credit_only_from_transmitted_cts);
  RUN_TEST(test_r44_full_size_with_deferred_grants);
  RUN_TEST(test_r45_unknown_codes_and_tid8);
  RUN_TEST(test_r46_cache_context_and_expiry);
  RUN_TEST(test_r44_ticket_identity);
  RUN_TEST(test_tid_all_taken_waits_and_quarantine_from_last_frame);
  RUN_TEST(test_tid_wrap_and_stale_serials);
  RUN_TEST(test_r49_old_lease_after_reset_or_new_owner);
  RUN_TEST(test_disc_trace_a_and_assign_rules);
  RUN_TEST(test_disc_want_conflict_release_and_new_gen);
  return UNITY_END();
}
