// lib/panel_core (task 027): one parser, §4.2 transmit rules, transport by own address,
// dispatch once from a complete message, boundaries, the panel-mode vehicle source.
// The D10 side is played with lib/zmb (Sender/Receiver, Side::Panel).
#include <stdio.h>
#include <string.h>
#include <unity.h>

#include <random>
#include <string>
#include <vector>

#include "panel_core.h"

using namespace panel_core;
using namespace zmb;

void setUp() {}
void tearDown() {}

static const uint8_t MAC[6] = {0x10, 0xBD, 0xA3, 0x5F, 0x51, 0x14};
static const uint32_t PANEL_ID = 0xCAFE0001;

static uint32_t xs(void* c) {
  uint32_t& x = *static_cast<uint32_t*>(c);
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  return x;
}

static uint32_t be32(const uint8_t* d) { return uint32_t(d[0]) << 24 | uint32_t(d[1]) << 16 | uint32_t(d[2]) << 8 | d[3]; }

static panel_rx::Stamp fresh(uint32_t now) {
  panel_rx::QueueStamp q;
  return q.read(q.poll(panel_rx::Snapshot::at(now, true, false)), now, false);
}

// Fake device command layer: counts executions, answers with the count.
struct Device {
  int executed = 0;
  std::vector<std::string> requests;
  bool answer = true;
};

struct World {
  uint32_t rng = 7;
  uint8_t rxbuf[kMaxMessage];
  uint8_t respbuf[kMaxMessage];
  TidEntry tids[64];
  Device dev;
  std::vector<std::pair<CanFrame, bool>> out;  // frames the core transmitted
  std::vector<std::string> logs;
  PanelCore* core = nullptr;
  uint32_t now = 1000;
  uint32_t panelGen = 0x11223344;
  uint8_t addr = 0;
  // D10 side
  Sender dtx{0, 0};
  uint8_t drxbuf[kMaxMessage];
  Receiver drx{0, drxbuf, sizeof drxbuf};
  uint8_t dmsg[kMaxMessage];
  std::vector<std::vector<uint8_t>> d10Got;  // messages the D10 received
  std::vector<Header> d10Hdr;
  uint16_t dtid = 100;
  uint32_t bootTag = 0;
  bool d10Mute = false;      // D10 side delivers nothing to the core (holds FLOW etc.)
  bool d10Deaf = false;      // D10 side ignores the core's XSTART/XDATA (its RESPONSE stays in flight)

  World() {
    Identity id;
    memcpy(id.mac, MAC, 6);
    for (int i = 0; i < 16; i++) id.bootId[i] = uint8_t(0xA0 + i);
    id.deviceId = "L8-10BDA35F5114";
    id.firmware = "test";
    Buffers b{rxbuf, sizeof rxbuf, respbuf, sizeof respbuf, tids, 64};
    CoreIo io;
    io.ctx = this;
    io.tx = [](void* c, const CanFrame& f, bool s) { static_cast<World*>(c)->out.push_back({f, s}); };
    io.dispatch = [](void* c, const uint8_t* req, size_t len, uint8_t* resp, size_t cap) -> size_t {
      World& w = *static_cast<World*>(c);
      w.dev.executed++;
      w.dev.requests.emplace_back(reinterpret_cast<const char*>(req), len);
      if (!w.dev.answer) return 0;
      std::string r = "{\"ok\":true,\"n\":" + std::to_string(w.dev.executed) + "}";
      if (r.size() > cap) return 0;
      memcpy(resp, r.data(), r.size());
      return r.size();
    };
    io.log = [](void* c, const char* t) { static_cast<World*>(c)->logs.push_back(t); };
    core = new PanelCore(id, b, io, xs, &rng);
    bootTag = be32(id.bootId);
  }
  ~World() { delete core; }

  void feed(const CanFrame& f) { core->onFrame(f, fresh(now), now); }

  CanFrame panelHb(uint32_t gen) {
    CanFrame f;
    f.id = panel_rx::kIdPanelHb;
    uint8_t d[8] = {uint8_t(PANEL_ID >> 24), uint8_t(PANEL_ID >> 16), uint8_t(PANEL_ID >> 8), uint8_t(PANEL_ID),
                    uint8_t(gen >> 24), uint8_t(gen >> 16), uint8_t(gen >> 8), uint8_t(gen)};
    memcpy(f.data, d, 8);
    return f;
  }
  CanFrame assign(uint8_t a, uint32_t gen) {
    CanFrame f;
    f.id = identityId(6, sfunc::kAssign, MAC);
    memcpy(f.data, MAC, 6);
    f.data[6] = a;
    f.data[7] = uint8_t(gen);
    return f;
  }

  RxContext dctx() const {
    RxContext c;
    c.local = 0;
    c.side = Side::Panel;
    c.panelGen = panelGen;
    c.havePeerBootTag = true;
    c.peerBootTag = bootTag;
    c.haveAssignedMac = true;
    memcpy(c.assignedMac, MAC, 6);
    return c;
  }

  // One millisecond: core timers, core -> D10, D10 -> core.
  void step() {
    now++;
    core->tick(now);
    for (size_t i = 0; i < out.size(); i++) {
      auto [f, single] = out[i];
      if (single) {
        core->singleShotResult(SingleShot::Sent, now);
        continue;
      }
      AddrId a;
      TFrame t;
      if (!parse(f, a, t) || a.dst != 0) continue;
      if (t.kind == Kind::Flow || t.kind == Kind::Ack || t.kind == Kind::RAbort) {
        dtx.onFrame(f, now);
      } else if (!d10Deaf) {
        RxOutput r = drx.onFrame(f, dctx(), now);
        if (r.hasReply && !d10Mute) {
          feed(r.reply);
          if (r.flow) drx.confirmFlow(r.flow, now);
        }
        if (r.event.kind == RxKind::Message) {
          d10Got.emplace_back(drx.message(), drx.message() + r.event.len);
          d10Hdr.push_back(r.event.header);
        }
      }
    }
    out.clear();
    dtx.tick(now);
    drx.tick(now);
    CanFrame f;
    while (!d10Mute && dtx.poll(now, f)) {
      dtx.confirm(now);
      feed(f);
    }
  }
  void run(int ms) {
    for (int i = 0; i < ms; i++) step();
  }

  // D10 sends a message of type t with payload, as a fresh transaction.
  void d10Send(MsgType t, const uint8_t* payload, size_t n, uint16_t reqTid = 0, uint32_t reqCrc = 0, int tid = -1) {
    Header h;
    h.mtype = uint8_t(t);
    h.tid = uint16_t(tid < 0 ? ++dtid : tid);
    h.reqTid = reqTid;
    h.panelGen = panelGen;
    h.bootTag = bootTag;
    h.reqCrc = reqCrc;
    h.encode(dmsg);
    memcpy(dmsg + kHeaderLen, payload, n);
    dtx = Sender(0, addr);
    TEST_ASSERT_TRUE(dtx.start(dmsg, kHeaderLen + n, h.tid));
  }
  void request(const char* json, int tid = -1) {
    d10Send(MsgType::Request, reinterpret_cast<const uint8_t*>(json), strlen(json), 0, 0, tid);
  }

  // PANEL_HB, ANNOUNCE, ASSIGN, HELLO, HELLO_ACK: the core is ready at address a.
  void bringUp(uint8_t a = 5) {
    feed(panelHb(panelGen));
    run(300);
    addr = a;
    drx = Receiver(a, drxbuf, sizeof drxbuf);
    feed(assign(a, panelGen));
    run(50);
    TEST_ASSERT_EQUAL(1, int(d10Got.size()));
    TEST_ASSERT_EQUAL(int(MsgType::Hello), d10Hdr[0].mtype);
    uint32_t crc = crc32(d10Got[0].data(), d10Got[0].size());
    HelloAck ack;
    ack.addr = a;
    ack.panelId = PANEL_ID;
    uint8_t p[8];
    size_t n = ack.encode(p);
    d10Send(MsgType::HelloAck, p, n, d10Hdr[0].tid, crc);
    run(50);
    TEST_ASSERT_TRUE(core->ready());
    d10Got.clear();
    d10Hdr.clear();
  }
};

static int countFunc(const std::vector<std::pair<CanFrame, bool>>& v, uint8_t fn) {
  int n = 0;
  for (auto& f : v) n += ((f.first.id >> 20) & 0x3F) == fn;
  return n;
}

// ---- C1: routing --------------------------------------------------------------------

static void test_c1_every_kind_reaches_only_its_consumer() {
  World w;
  w.bringUp();
  Counters c0 = w.core->counters();
  CanFrame veh;
  veh.id = panel_rx::kIdVehFast;
  uint8_t vd[8] = {0x09, 0xC4, 0, 0, 0, 0x01, 2, 1};
  memcpy(veh.data, vd, 8);
  w.feed(veh);
  TEST_ASSERT_EQUAL(2500, w.core->panel().field(panel_rx::kRpm).value);
  TEST_ASSERT_EQUAL(c0.panel + 1, w.core->counters().panel);
  CanFrame std11;
  std11.id = 0x123;
  std11.extended = false;
  w.feed(std11);
  CanFrame rtr;
  rtr.id = 0x11100500;
  rtr.rtr = true;
  w.feed(rtr);
  CanFrame dlc;
  dlc.id = AddrId{4, func::kXdata, 0, 5, 1}.encode();
  dlc.dlc = 7;
  w.feed(dlc);
  CanFrame other;
  other.id = AddrId{4, func::kXdata, 0, 6, 1}.encode();  // another module
  w.feed(other);
  CanFrame fromModule;
  fromModule.id = AddrId{4, func::kXstart, 9, 5, 1}.encode();  // src not D10
  w.feed(fromModule);
  CanFrame foreign;
  foreign.id = AddrId{5, 0x18, 7, 0x3F, 0}.encode();  // someone's STATUS_A
  w.feed(foreign);
  const Counters& c = w.core->counters();
  TEST_ASSERT_EQUAL(c0.drop_std + 1, c.drop_std);
  TEST_ASSERT_EQUAL(c0.drop_rtr + 1, c.drop_rtr);
  TEST_ASSERT_EQUAL(c0.drop_dlc + 1, c.drop_dlc);
  TEST_ASSERT_EQUAL(c0.drop_other_dst + 1, c.drop_other_dst);
  TEST_ASSERT_EQUAL(c0.drop_other_src + 1, c.drop_other_src);
  TEST_ASSERT_EQUAL(c0.drop_not_ours + 1, c.drop_not_ours);
  TEST_ASSERT_EQUAL(c0.transport, c.transport);
  TEST_ASSERT_EQUAL(0, w.dev.executed);
  TEST_ASSERT_TRUE(w.out.empty());  // none of them made the core transmit
}

// ---- C2: transmit rules before/after ASSIGN -------------------------------------------

static void test_c2_before_assign_only_announce_and_beacon() {
  World w;
  w.feed(w.panelHb(w.panelGen));
  std::vector<std::pair<CanFrame, bool>> all;
  for (int i = 0; i < 3000; i++) {
    w.now++;
    w.core->tick(w.now);
    for (auto& f : w.out) {
      all.push_back(f);
      if (f.second) w.core->singleShotResult(SingleShot::Sent, w.now);
    }
    w.out.clear();
  }
  TEST_ASSERT_TRUE(countFunc(all, sfunc::kAnnounce) >= 2);
  for (auto& f : all) {
    TEST_ASSERT_TRUE(f.second);
    TEST_ASSERT_EQUAL(sfunc::kAnnounce, (f.first.id >> 20) & 0x3F);
  }
  // a request addressed to "us" before any address: dropped, nothing sent
  CanFrame x;
  x.id = AddrId{4, func::kXstart, 0, 5, 1}.encode();
  w.feed(x);
  TEST_ASSERT_EQUAL(1, w.core->counters().drop_unassigned);
  TEST_ASSERT_TRUE(w.out.empty());
  // after ASSIGN: HELLO (transport to D10), then STATUS_A once ready
  w.addr = 5;
  w.drx = Receiver(5, w.drxbuf, sizeof w.drxbuf);
  w.feed(w.assign(5, w.panelGen));
  TEST_ASSERT_TRUE(countFunc(w.out, func::kXstart) == 1);
}

static void test_c2_status_a_only_when_ready() {
  World w;
  w.bringUp();
  int status = 0;
  for (int i = 0; i < 1200; i++) {
    w.now++;
    w.core->tick(w.now);
    status += countFunc(w.out, sfunc::kStatusA);
    w.out.clear();
  }
  TEST_ASSERT_TRUE(status >= 2);
}

// ---- C3/C4: own address, dispatch once, the reply is the RESPONSE ------------------------

static void test_c4_request_dispatched_once_reply_is_response() {
  World w;
  w.bringUp();
  w.request("{\"v\":1,\"id\":\"r1\",\"cmd\":\"status\"}");
  uint32_t reqCrc = crc32(w.dmsg, kHeaderLen + strlen("{\"v\":1,\"id\":\"r1\",\"cmd\":\"status\"}"));
  w.run(100);
  TEST_ASSERT_EQUAL(1, w.dev.executed);
  TEST_ASSERT_EQUAL_STRING("{\"v\":1,\"id\":\"r1\",\"cmd\":\"status\"}", w.dev.requests[0].c_str());
  TEST_ASSERT_EQUAL(1, int(w.d10Got.size()));
  Header h = w.d10Hdr[0];
  TEST_ASSERT_EQUAL(int(MsgType::Response), h.mtype);
  TEST_ASSERT_EQUAL(w.dtid, h.reqTid);
  TEST_ASSERT_EQUAL_UINT32(reqCrc, h.reqCrc);
  TEST_ASSERT_EQUAL_UINT32(w.panelGen, h.panelGen);
  TEST_ASSERT_EQUAL_UINT32(w.bootTag, h.bootTag);
  std::string body(w.d10Got[0].begin() + kHeaderLen, w.d10Got[0].end());
  TEST_ASSERT_EQUAL_STRING("{\"ok\":true,\"n\":1}", body.c_str());
  TEST_ASSERT_EQUAL(1, w.core->counters().responses_sent);
}

static void test_c3_transport_for_another_address_changes_nothing() {
  World w;
  w.bringUp(5);
  // the same request addressed to module 6: every frame dropped
  w.addr = 6;
  w.request("{\"v\":1,\"id\":\"x\",\"cmd\":\"info\"}");
  w.run(200);
  TEST_ASSERT_EQUAL(0, w.dev.executed);
  TEST_ASSERT_TRUE(w.core->counters().drop_other_dst > 0);
}

// ---- C5: not twice ----------------------------------------------------------------------

static void test_c5_repeated_request_not_executed_again() {
  World w;
  w.bringUp();
  const char* j = "{\"v\":1,\"id\":\"r1\",\"cmd\":\"preview\"}";
  w.request(j, 500);
  w.run(100);
  TEST_ASSERT_EQUAL(1, w.dev.executed);
  // a late copy of the same transaction after the receiver's 1 s window
  w.run(1500);
  w.request(j, 500);
  w.run(200);
  TEST_ASSERT_EQUAL(1, w.dev.executed);
  TEST_ASSERT_EQUAL(1, w.core->counters().dup_request);
  // a new transaction with the same body is a new request
  w.request(j, 501);
  w.run(200);
  TEST_ASSERT_EQUAL(2, w.dev.executed);
}

static void test_c5_busy_request_not_executed() {
  World w;
  w.bringUp();
  w.d10Deaf = true;  // the RESPONSE's XSTART gets no FLOW: it stays in flight up to N_Bs
  w.request("{\"v\":1,\"id\":\"a\",\"cmd\":\"status\"}");
  for (int i = 0; i < 20; i++) w.step();
  TEST_ASSERT_EQUAL(1, w.dev.executed);
  w.request("{\"v\":1,\"id\":\"b\",\"cmd\":\"status\"}");
  for (int i = 0; i < 20; i++) w.step();
  TEST_ASSERT_EQUAL(1, w.dev.executed);
  TEST_ASSERT_EQUAL(1, w.core->counters().busy_dropped);
  // once that send has ended (N_Bs), requests are served again
  w.d10Deaf = false;
  w.run(300);
  w.request("{\"v\":1,\"id\":\"c\",\"cmd\":\"status\"}");
  w.run(100);
  TEST_ASSERT_EQUAL(2, w.dev.executed);
}

// ---- C6: boundaries ---------------------------------------------------------------------

static void test_c6_new_panelgen_mid_assembly_executes_nothing() {
  World w;
  w.bringUp();
  std::string big = "{\"v\":1,\"id\":\"big\",\"cmd\":\"apply\",\"pad\":\"" + std::string(600, 'x') + "\"}";
  w.request(big.c_str());
  for (int i = 0; i < 6; i++) w.step();  // part of the XDATA
  TEST_ASSERT_EQUAL(0, w.dev.executed);
  uint32_t b0 = w.core->counters().boundaries;
  w.panelGen = 0x55667788;
  w.feed(w.panelHb(w.panelGen));
  TEST_ASSERT_TRUE(w.core->counters().boundaries > b0);
  TEST_ASSERT_FALSE(w.core->ready());
  w.run(300);  // the rest of the old message keeps coming
  TEST_ASSERT_EQUAL(0, w.dev.executed);
}

static void test_c6_release_and_duplicate_address() {
  World w;
  w.bringUp();
  w.feed(w.assign(0, w.panelGen));  // RELEASE
  TEST_ASSERT_FALSE(w.core->ready());
  TEST_ASSERT_EQUAL(0, w.core->addr());
  w.request("{\"v\":1,\"id\":\"r\",\"cmd\":\"status\"}");
  w.run(200);
  TEST_ASSERT_EQUAL(0, w.dev.executed);

  World v;
  v.bringUp(7);
  // our address in another MAC's BEACON
  CanFrame b;
  uint8_t other[6] = {0x10, 0xBD, 0xA3, 0x5F, 0x99, 0x99};
  b.id = identityId(5, sfunc::kBeacon, other);
  b.data[0] = 7;
  memcpy(b.data + 1, other, 6);
  v.feed(b);
  TEST_ASSERT_FALSE(v.core->ready());
  v.request("{\"v\":1,\"id\":\"r\",\"cmd\":\"status\"}");
  v.run(200);
  TEST_ASSERT_EQUAL(0, v.dev.executed);
}

static void test_c6_hello_timeout_starts_over() {
  World w;
  w.feed(w.panelHb(w.panelGen));
  w.run(300);
  w.addr = 5;
  w.drx = Receiver(5, w.drxbuf, sizeof w.drxbuf);
  w.feed(w.assign(5, w.panelGen));
  w.run(2200);  // no HELLO_ACK
  TEST_ASSERT_EQUAL(1, w.core->counters().hello_timeouts);
  TEST_ASSERT_FALSE(w.core->ready());
}

static void test_c6_not_ready_request_not_executed() {
  World w;
  w.feed(w.panelHb(w.panelGen));
  w.run(300);
  w.addr = 5;
  w.drx = Receiver(5, w.drxbuf, sizeof w.drxbuf);
  w.feed(w.assign(5, w.panelGen));
  w.run(50);
  w.request("{\"v\":1,\"id\":\"r\",\"cmd\":\"status\"}");  // before HELLO_ACK
  w.run(100);
  TEST_ASSERT_EQUAL(0, w.dev.executed);
  TEST_ASSERT_EQUAL(1, w.core->counters().not_ready);
}

// ---- C7: vehicle source -----------------------------------------------------------------

// The existing rules, vehicle.cpp:200-201 and 242 (DATA_STALE 3000), on the view.
static bool existingFresh(const VehicleView& v, uint32_t now) { return v.lastUpdate && now - v.lastUpdate <= 3000; }
static bool existingRunning(const VehicleView& v, uint32_t now) { return existingFresh(v, now) && v.rpm > 300; }

static CanFrame vehFast(uint16_t rpm, uint8_t valid, uint8_t age, uint8_t seq) {
  CanFrame f;
  f.id = panel_rx::kIdVehFast;
  uint8_t d[8] = {uint8_t(rpm >> 8), uint8_t(rpm), 0x02, 0x0B, 17, valid, age, seq};
  memcpy(f.data, d, 8);
  return f;
}

static void test_c7_vehicle_view_matches_existing_rule() {
  World w;
  uint8_t seq = 1;
  // running
  for (int i = 0; i < 50; i++) {
    w.now += 20;
    w.feed(vehFast(2500, 0x07, 3, seq++));
  }
  VehicleView v = w.core->panelVehicle(w.now);
  TEST_ASSERT_EQUAL(2500, v.rpm);
  TEST_ASSERT_EQUAL(52, v.speed);
  TEST_ASSERT_EQUAL(17, v.throttle);
  TEST_ASSERT_TRUE(existingRunning(v, w.now));
  TEST_ASSERT_EQUAL(w.core->panel().engineRunning(w.now), existingRunning(v, w.now));
  // ECU freeze: rpm without valid from now on
  uint32_t t0 = w.now;
  bool switched = false;
  for (int i = 0; i < 200; i++) {
    w.now += 20;
    w.feed(vehFast(0, 0x02, 1, seq++));
    v = w.core->panelVehicle(w.now);
    TEST_ASSERT_EQUAL(w.core->panel().engineRunning(w.now), existingRunning(v, w.now));
    TEST_ASSERT_EQUAL(w.core->panel().vehicleDataFresh(w.now), existingFresh(v, w.now));
    if (!existingRunning(v, w.now) && !switched) {
      switched = true;
      TEST_ASSERT_TRUE(w.now - t0 <= 3000 + 22);
      TEST_ASSERT_TRUE(w.now - t0 >= 3000 - 30);
    }
  }
  TEST_ASSERT_TRUE(switched);
  // dashcore freeze / link loss: no frames at all, the same rule
  World u;
  u.feed(vehFast(1800, 0x01, 1, 1));
  for (uint32_t dt = 0; dt < 4000; dt += 10) {
    VehicleView x = u.core->panelVehicle(u.now + dt);
    TEST_ASSERT_EQUAL(u.core->panel().engineRunning(u.now + dt), existingRunning(x, u.now + dt));
  }
  TEST_ASSERT_FALSE(existingRunning(u.core->panelVehicle(u.now + 3100), u.now + 3100));
  // no data at all
  World z;
  TEST_ASSERT_FALSE(existingFresh(z.core->panelVehicle(5000), 5000));
}

// ---- C8: bad input ----------------------------------------------------------------------

static void test_c8_fuzz_never_dispatches_from_garbage() {
  World w;
  w.bringUp();
  std::mt19937 rng(27);
  for (int i = 0; i < 200000; i++) {
    CanFrame f;
    uint32_t k = rng() % 8;
    if (k < 4) f.id = AddrId{uint8_t(rng() % 8), uint8_t(0x08 + rng() % 11), uint8_t(rng() % 2 ? 0 : rng() % 64),
                             uint8_t(rng() % 2 ? 5 : rng() % 64), uint8_t(rng())}.encode();
    else f.id = rng() & 0x1FFFFFFF;
    f.extended = rng() % 16 != 0;
    f.rtr = rng() % 32 == 0;
    f.dlc = uint8_t(rng() % 4 == 0 ? rng() % 9 : 8);
    for (auto& b : f.data) b = uint8_t(rng());
    w.now += rng() % 3;
    w.feed(f);
    if (i % 50 == 0) w.core->tick(w.now);
    w.out.clear();
  }
  // a random sequence can only finish a message through a valid CRC-32: practically never
  TEST_ASSERT_EQUAL(0, w.dev.executed);
  printf("C8: transport %u dispatched %u requests %u other_dst %u other_src %u not_ours %u\n", w.core->counters().transport,
         w.core->counters().dispatched, w.core->counters().requests, w.core->counters().drop_other_dst,
         w.core->counters().drop_other_src, w.core->counters().drop_not_ours);
}

static void test_c9_sizes() {
  printf("sizes: PanelCore %zu bytes (+ caller buffers rx %zu, resp %zu, tid ring %zu x %zu)\n", sizeof(PanelCore),
         size_t(kMaxMessage), size_t(kMaxMessage), size_t(64), sizeof(TidEntry));
  TEST_ASSERT_TRUE(sizeof(PanelCore) <= 1024);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_c1_every_kind_reaches_only_its_consumer);
  RUN_TEST(test_c2_before_assign_only_announce_and_beacon);
  RUN_TEST(test_c2_status_a_only_when_ready);
  RUN_TEST(test_c4_request_dispatched_once_reply_is_response);
  RUN_TEST(test_c3_transport_for_another_address_changes_nothing);
  RUN_TEST(test_c5_repeated_request_not_executed_again);
  RUN_TEST(test_c5_busy_request_not_executed);
  RUN_TEST(test_c6_new_panelgen_mid_assembly_executes_nothing);
  RUN_TEST(test_c6_release_and_duplicate_address);
  RUN_TEST(test_c6_hello_timeout_starts_over);
  RUN_TEST(test_c6_not_ready_request_not_executed);
  RUN_TEST(test_c7_vehicle_view_matches_existing_rule);
  RUN_TEST(test_c8_fuzz_never_dispatches_from_garbage);
  RUN_TEST(test_c9_sizes);
  return UNITY_END();
}
