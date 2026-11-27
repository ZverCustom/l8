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
        if (o.hasReply) { TxOutcome r = l.tx.onFrame(o.reply, l.now); if (r.result != TxResult::None) l.out = r; }
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
  TEST_ASSERT_TRUE(o.hasReply);
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
  return UNITY_END();
}
