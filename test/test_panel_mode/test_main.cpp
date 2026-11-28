// L8 panel mode in the firmware (task 036, docs/PANEL_MODE.md): src/panel_mode/PanelMode over
// a model TWAI port, with the REAL command layer l8proto (as tools/panel_node.cpp), the mode
// plan of the CAN modes, and the readers' rules (src/vehicle/vehicle_rules.h). The D10 side
// is played with lib/zmb (Sender/Receiver, Side::Panel), as in test_panel_core.
#include <Arduino.h>
#include <LittleFS.h>
#include <unity.h>

#include <deque>
#include <string>
#include <vector>

#include "../../src/settings/settings.cpp"
#include "../../src/protocol/protocol.cpp"
#include "../../src/panel_mode/panel_mode.cpp"
#include "vehicle/mode_plan.h"
#include "vehicle/vehicle_rules.h"

SerialStub Serial;
uint32_t testMillis = 0;
FakeFS LittleFS;
CanSettings can_setting;
StripSettings stripSettings[NUM_STRIPS];
int numLedsPerStrip[NUM_STRIPS];
int canMode = 2, canBitrateSel = 1;
String wifiSsid, wifiPass;
const char* netDefaultSsid() { return ""; }
const char* netDefaultPass() { return ""; }
void vehicleSetMode(int n) { canMode = n; }
void vehicleSetBitrate(int n) { canBitrateSel = n; }
void ledsPause() {}
void ledsResume() {}
void ledsFlashPause() {}
void ledsFlashResume() {}
void requestLedReinit() {}
void powerTick() {}

using namespace zmb;
using panel_core::SingleShot;
using panel_core::VehicleView;

void setUp() {}
void tearDown() {}

static const uint8_t MAC[6] = {0x10, 0xBD, 0xA3, 0x5F, 0x51, 0x14};
static const uint32_t PANEL_ID = 0xCAFE0036;

// ---- the model TWAI port and NVS -------------------------------------------------------------

struct ModelPort : panel_mode::PanelPort {
  std::deque<CanFrame> rxq;
  std::vector<std::pair<CanFrame, bool>> sent;
  uint32_t overruns = 0;
  bool off = false, failSends = false;
  bool listenOnly = false;
  int toListen = 0, toNormal = 0, sentCount = 0;
  bool everNormal = false;
  bool single = false;
  bool singleResultReady = false;
  SingleShot singleResult = SingleShot::Sent;
  bool read(CanFrame& f) override {
    if (rxq.empty()) return false;
    f = rxq.front();
    rxq.pop_front();
    return true;
  }
  bool send(const CanFrame& f, bool s) override {
    TEST_ASSERT_FALSE_MESSAGE(listenOnly, "a frame offered to the driver in listen-only");
    if (off || failSends) return false;
    sentCount++;
    sent.push_back({f, s});
    if (s) single = true;
    return true;
  }
  bool rxQueueEmpty() override { return rxq.empty(); }
  uint32_t overrunCount() override { return overruns; }
  bool busOff() override { return off; }
  void setListenOnly(bool l) override {
    listenOnly = l;
    (l ? toListen : toNormal)++;
    everNormal |= !l;
    if (l) off = false;  // the driver restarts: out of bus-off
  }
  bool singleShotDone(SingleShot& r) override {
    if (!single || !singleResultReady) return false;
    single = false;
    r = singleResult;
    return true;
  }
};

struct ModelStore : panel_mode::PanelStore {
  uint32_t id = 0;
  uint8_t addr = 0;
  int saves = 0;
  bool load(uint32_t& i, uint8_t& a) override {
    i = id;
    a = addr;
    return id && addr;
  }
  void save(uint32_t i, uint8_t a) override {
    id = i;
    addr = a;
    saves++;
  }
};

static uint32_t xs(void* c) {
  uint32_t& x = *static_cast<uint32_t*>(c);
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  return x;
}

// ---- the world: PanelMode + the D10 side --------------------------------------------------------

struct World {
  ModelPort port;
  ModelStore store;
  uint32_t now = 1000, rng = 36;
  int dispatched = 0;
  uint8_t statusFlagsExtra = 0;  // what the firmware adds (ioOk, anyFault)
  panel_mode::PanelMode* mode = nullptr;
  uint8_t bootId[16];
  uint32_t panelGen = 0x11223344;
  uint8_t addr = 0;
  Sender dtx{0, 0};
  uint8_t drxbuf[kMaxMessage];
  Receiver drx{0, drxbuf, sizeof drxbuf};
  uint8_t dmsg[kMaxMessage];
  std::vector<std::vector<uint8_t>> d10Got;
  std::vector<Header> d10Hdr;
  std::vector<CanFrame> statusA;
  uint16_t dtid = 100;
  uint32_t bootTag = 0;
  bool hbOn = true;  // the panel's PANEL_HB every 100 ms

  World() {
    settingsBegin();
    panel_mode::Host h;
    h.ctx = this;
    h.now = [](void* c) { return static_cast<World*>(c)->now; };
    h.dispatch = [](void* c, const uint8_t* req, size_t len, uint8_t* resp, size_t cap) -> size_t {
      World& w = *static_cast<World*>(c);
      w.dispatched++;
      testMillis = w.now;
      int http = 0;
      String r = l8proto::handleV1(reinterpret_cast<const char*>(req), len, l8proto::Transport::Usb, true, http);
      if (r.length() > cap) return 0;
      memcpy(resp, r.c_str(), r.length());
      return r.length();
    };
    h.statusA = [](void* c, uint8_t* flags, uint16_t* rev, uint16_t* saved, uint8_t* chOn, uint8_t* chFault) {
      World& w = *static_cast<World*>(c);
      bool rpmFresh = w.mode->view(w.now).fresh & panel_core::kFreshRpm;
      uint32_t sv = 0;
      bool hs = settingsSavedRevision(sv);
      *flags = uint8_t((drive.masterOn ? 1 : 0) | (settingsPreviewActive() ? 2 : 0) | w.statusFlagsExtra | (rpmFresh ? 16 : 0));
      *rev = uint16_t(settingsRevision());
      *saved = hs ? uint16_t(sv) : 0xFFFF;
      *chOn = 0;
      *chFault = 0;
    };
    h.rand = xs;
    h.randCtx = &rng;
    mode = new panel_mode::PanelMode(port, store, h);
    boot(0xA0);
  }
  ~World() { delete mode; }

  void boot(uint8_t seed) {
    for (int i = 0; i < 16; i++) bootId[i] = uint8_t(seed + i);
    bootTag = uint32_t(bootId[0]) << 24 | uint32_t(bootId[1]) << 16 | uint32_t(bootId[2]) << 8 | bootId[3];
    mode->enter(MAC, 1, bootId, "L8-10BDA35F5114", "test", 3);
  }

  void feed(const CanFrame& f) { port.rxq.push_back(f); }

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

  // One millisecond: the mode's loop pass, what it sent reaches the D10, the D10 answers.
  void step() {
    now++;
    if (hbOn && now % 100 == 0) feed(panelHb(panelGen));
    mode->tick();
    auto out = port.sent;
    port.sent.clear();
    for (auto& [f, single] : out) {
      if (single) {
        port.singleResultReady = true;
        continue;
      }
      if (((f.id >> 20) & 0x3F) == sfunc::kStatusA) {
        statusA.push_back(f);
        continue;
      }
      AddrId a;
      TFrame t;
      if (!parse(f, a, t)) continue;
      if (a.dst != 0) continue;
      if (t.kind == Kind::Flow || t.kind == Kind::Ack || t.kind == Kind::RAbort) {
        dtx.onFrame(f, now);
      } else {
        RxOutput r = drx.onFrame(f, dctx(), now);
        if (r.hasReply) {
          feed(r.reply);
          if (r.flow) drx.confirmFlow(r.flow, now);
        }
        if (r.event.kind == RxKind::Message) {
          d10Got.emplace_back(drx.message(), drx.message() + r.event.len);
          d10Hdr.push_back(r.event.header);
        }
      }
    }
    dtx.tick(now);
    drx.tick(now);
    CanFrame f;
    while (dtx.poll(now, f)) {
      dtx.confirm(now);
      feed(f);
    }
  }
  void run(int ms) {
    for (int i = 0; i < ms; i++) step();
  }

  void d10Send(MsgType t, const uint8_t* payload, size_t n, uint16_t reqTid = 0, uint32_t reqCrc = 0) {
    Header h;
    h.mtype = uint8_t(t);
    h.tid = ++dtid;
    h.reqTid = reqTid;
    h.panelGen = panelGen;
    h.bootTag = bootTag;
    h.reqCrc = reqCrc;
    h.encode(dmsg);
    memcpy(dmsg + kHeaderLen, payload, n);
    dtx = Sender(0, addr);
    TEST_ASSERT_TRUE(dtx.start(dmsg, kHeaderLen + n, h.tid));
  }
  void request(const char* json) { d10Send(MsgType::Request, reinterpret_cast<const uint8_t*>(json), strlen(json)); }

  void bringUp(uint8_t a = 5) {
    run(400);  // 3 PANEL_HB: normal mode (A5)
    TEST_ASSERT_FALSE(mode->listening());
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
    TEST_ASSERT_TRUE(mode->core()->ready());
    d10Got.clear();
    d10Hdr.clear();
  }
};

static CanFrame vehFast(uint16_t rpm, uint8_t valid, uint8_t age, uint8_t seq, uint16_t speed10 = 523, uint8_t thr = 17) {
  CanFrame f;
  f.id = panel_rx::kIdVehFast;
  uint8_t d[8] = {uint8_t(rpm >> 8), uint8_t(rpm), uint8_t(speed10 >> 8), uint8_t(speed10), thr, valid, age, seq};
  memcpy(f.data, d, 8);
  return f;
}
static CanFrame vehSlow(uint8_t coolant, uint8_t valid, uint8_t age, uint8_t seq) {
  CanFrame f;
  f.id = panel_rx::kIdVehSlow;
  uint8_t d[8] = {coolant, 0xFF, 0, 1, valid, age, seq, 0};
  memcpy(f.data, d, 8);
  return f;
}

// ---- D1: the CAN mode plan: old rows unchanged, panel only in row 4 ----------------------------

static void test_d1_old_modes_plan_unchanged_panel_off() {
  for (int mode : {0, 1, 2, 3, 5, 7, -1, 99}) {
    vmode::Plan p = vmode::plan(mode);
    // the expressions vehicle.cpp had at 8849a5c
    bool oldListen = mode != 0;
    bool oldScan = mode != 0 && mode != 3;
    TEST_ASSERT_EQUAL_MESSAGE(oldListen, p.listenOnly, "listen-only");
    TEST_ASSERT_EQUAL_MESSAGE(oldScan, p.autoBitrate, "auto bitrate");
    TEST_ASSERT_EQUAL(mode == 0, p.obd);
    TEST_ASSERT_EQUAL(mode == 3, p.kline);
    TEST_ASSERT_EQUAL(mode != 0 && mode != 3, p.readFrames);
    TEST_ASSERT_FALSE_MESSAGE(p.panel, "no new path with old settings");
    for (int sel = 0; sel <= 5; sel++) TEST_ASSERT_EQUAL(sel ? sel - 1 : 0, vmode::bitrateFor(mode, sel));
  }
  vmode::Plan p = vmode::plan(4);
  TEST_ASSERT_TRUE(p.panel);
  TEST_ASSERT_TRUE(p.listenOnly);  // A5 (R92): the mode starts listen-only
  TEST_ASSERT_FALSE(p.autoBitrate);
  TEST_ASSERT_FALSE(p.obd);
  TEST_ASSERT_FALSE(p.readFrames);
  for (int sel = 0; sel <= 5; sel++) TEST_ASSERT_EQUAL(vmode::kBr1M, vmode::bitrateFor(4, sel));
}

// ---- D2: the mode itself ---------------------------------------------------------------------------

static void test_d2_discovery_address_and_stored_address() {
  World w;
  w.bringUp(5);
  TEST_ASSERT_EQUAL_STRING("ready", w.mode->phase());
  w.run(5);
  TEST_ASSERT_EQUAL(1, w.store.saves);
  TEST_ASSERT_EQUAL_UINT32(PANEL_ID, w.store.id);
  TEST_ASSERT_EQUAL(5, w.store.addr);
  // reboot: a new boot of the mode keeps the stored address for discovery
  w.mode->exit();
  TEST_ASSERT_EQUAL_STRING("off", w.mode->phase());
  w.boot(0xB0);
  TEST_ASSERT_EQUAL(5, w.mode->core()->discovery().storedAddr());
  TEST_ASSERT_EQUAL_STRING("listen", w.mode->phase());  // A5: every entry starts listening
}

static void test_d2_request_through_the_real_dispatcher() {
  World w;
  w.bringUp();
  const char* req = "{\"v\":1,\"id\":\"p1\",\"cmd\":\"info\"}";
  w.request(req);
  w.run(150);
  TEST_ASSERT_EQUAL(1, w.dispatched);
  TEST_ASSERT_EQUAL(1, int(w.d10Got.size()));
  TEST_ASSERT_EQUAL(int(MsgType::Response), w.d10Hdr[0].mtype);
  std::string body(w.d10Got[0].begin() + kHeaderLen, w.d10Got[0].end());
  // the RESPONSE is exactly what the same dispatcher answers directly (HTTP/USB parity)
  int http = 0;
  String direct = l8proto::handleV1(req, strlen(req), l8proto::Transport::Usb, true, http);
  TEST_ASSERT_EQUAL_STRING(direct.c_str(), body.c_str());
  TEST_ASSERT_TRUE_MESSAGE(body.find("\"ok\":true") != std::string::npos, body.c_str());
  // a mutation: preview through the bus changes the device's state once
  uint32_t rev = settingsRevision();
  w.request("{\"v\":1,\"id\":\"p2\",\"cmd\":\"preview\",\"client\":\"bus\",\"revision\":0,\"patch\":{\"strips\":[{\"brightness\":40}]}}");
  w.run(150);
  TEST_ASSERT_EQUAL(2, w.dispatched);
  std::string b2(w.d10Got.back().begin() + kHeaderLen, w.d10Got.back().end());
  TEST_ASSERT_TRUE_MESSAGE(b2.find("\"id\":\"p2\"") != std::string::npos, b2.c_str());
  TEST_ASSERT_TRUE_MESSAGE(settingsRevision() != rev || b2.find("\"ok\":false") != std::string::npos, b2.c_str());
}

static void test_d2_veh_per_field_freshness_and_status_a_rpm_fresh() {
  World w;
  w.bringUp();
  w.statusFlagsExtra = 4;  // ioOk
  uint8_t seqF = 1, seqS = 1;
  // rpm, speed, throttle valid; coolant valid
  for (int i = 0; i < 30; i++) {
    w.feed(vehFast(2500, 7, 2, seqF++));
    if (i % 5 == 0) w.feed(vehSlow(130, 1, 2, seqS++));
    w.run(20);
  }
  VehicleView v = w.mode->view(w.now);
  TEST_ASSERT_EQUAL(2500, v.rpm);
  TEST_ASSERT_EQUAL(52, v.speed);
  TEST_ASSERT_EQUAL(90, v.temp);
  TEST_ASSERT_EQUAL(panel_core::kFreshRpm | panel_core::kFreshSpeed | panel_core::kFreshThrottle | panel_core::kFreshTemp, v.fresh);
  w.statusA.clear();
  w.run(1100);  // no VEH for 1.1 s: everything stale
  v = w.mode->view(w.now);
  TEST_ASSERT_EQUAL(0, v.fresh);
  TEST_ASSERT_TRUE(w.statusA.size() >= 2);
  // the last STATUS_A: rpmFresh clear, ioOk set; one sent while fresh had it set
  TEST_ASSERT_EQUAL(0, w.statusA.back().data[0] & 16);
  TEST_ASSERT_EQUAL(4, w.statusA.back().data[0] & 4);
  w.statusA.clear();
  for (int i = 0; i < 40; i++) {
    w.feed(vehFast(2500, 1, 2, seqF++));
    w.run(20);
  }
  bool sawFresh = false;
  for (auto& f : w.statusA) sawFresh |= (f.data[0] & 16) != 0;
  TEST_ASSERT_TRUE_MESSAGE(sawFresh, "STATUS_A rpmFresh from the view");
}

// ---- R92 / A5: listen first ---------------------------------------------------------------------

static void test_l1_no_panel_hb_60_s_nothing_sent_never_normal() {
  World w;
  w.hbOn = false;
  uint32_t r = 99, alt = 0;
  auto rnd = [&](uint32_t n) { return xs(&r) % n; };
  for (int ms = 0; ms < 60000; ms++) {
    if (ms % 10 == 0) {
      CanFrame f;
      switch (rnd(8)) {
        case 0:  // standard IDs (a car bus)
          f.extended = false;
          f.id = 0x316 + rnd(0x400);
          break;
        case 1:  // other extended IDs
          f.id = rnd(0x1FFFFFFF);
          break;
        case 2: {  // PANEL_HB of the wrong length
          f = w.panelHb(w.panelGen);
          f.dlc = uint8_t(rnd(8));
          break;
        }
        case 3: {  // PANEL_HB with panelId 0
          f = w.panelHb(w.panelGen);
          memset(f.data, 0, 4);
          break;
        }
        case 4: {  // other panels: never the real one, never twice the same in a row
          f = w.panelHb(w.panelGen + 1 + (alt % 2));
          f.data[3] ^= uint8_t(1 + (alt++ % 3));
          break;
        }
        case 5:  // a remote frame with the PANEL_HB ID
          f = w.panelHb(w.panelGen);
          f.rtr = true;
          break;
        default:
          for (auto& b : f.data) b = uint8_t(rnd(256));
          f.id = rnd(0x1FFFFFFF);
          f.dlc = uint8_t(rnd(9));
      }
      w.feed(f);
    }
    if (ms % 1000 == 500) {  // two correct PANEL_HB, then nothing for 900 ms
      w.feed(w.panelHb(w.panelGen));
    }
    if (ms % 1000 == 600) w.feed(w.panelHb(w.panelGen));
    w.now++;
    w.mode->tick();
  }
  TEST_ASSERT_EQUAL_MESSAGE(0, w.port.sentCount, "not one frame in 60 s without a panel");
  TEST_ASSERT_FALSE_MESSAGE(w.port.everNormal, "never left listen-only");
  TEST_ASSERT_TRUE(w.mode->listening());
  TEST_ASSERT_EQUAL_STRING("listen", w.mode->phase());
  TEST_ASSERT_TRUE(w.mode->counters().frames > 5000);
}

static void test_l2_three_panel_hb_then_normal_and_discovery() {
  World w;
  w.hbOn = false;
  for (int k = 0; k < 2; k++) {
    w.feed(w.panelHb(w.panelGen));
    w.run(100);
  }
  TEST_ASSERT_TRUE(w.mode->listening());
  TEST_ASSERT_EQUAL(2, w.mode->hbCount());
  TEST_ASSERT_EQUAL(0, w.port.sentCount);
  w.feed(w.panelHb(w.panelGen));
  w.run(1);
  TEST_ASSERT_FALSE(w.mode->listening());
  TEST_ASSERT_FALSE(w.port.listenOnly);
  w.hbOn = true;
  w.bringUp(5);  // the previous discovery path
  TEST_ASSERT_EQUAL_STRING("ready", w.mode->phase());
}

static void test_l3_panel_hb_lost_back_to_listen() {
  World w;
  w.bringUp();
  w.hbOn = false;
  w.run(1000);
  TEST_ASSERT_FALSE(w.mode->listening());
  w.run(2);
  TEST_ASSERT_TRUE(w.mode->listening());
  TEST_ASSERT_TRUE(w.mode->listenReason() == panel_mode::Listen::NoHb);
  int sent = w.port.sentCount;
  w.run(5000);
  TEST_ASSERT_EQUAL_MESSAGE(sent, w.port.sentCount, "nothing sent after the panel was lost");
  w.hbOn = true;
  w.run(400);
  TEST_ASSERT_FALSE_MESSAGE(w.mode->listening(), "the panel is back: normal again");
}

// one bus-off in normal mode; returns ms from the bus-off to normal again (PANEL_HB flowing)
static uint32_t busOffAndBack(World& w) {
  TEST_ASSERT_FALSE(w.mode->listening());
  w.port.off = true;
  w.run(1);
  TEST_ASSERT_TRUE(w.mode->listening());
  uint32_t t0 = w.now;
  // (ModelPort::send fails the test if a frame is offered while listen-only)
  while (w.mode->listening()) {
    w.run(1);
    TEST_ASSERT_TRUE_MESSAGE(w.now - t0 < 70000, "never back");
  }
  return w.now - t0;
}

static void test_l4_bus_off_listen_backoff_and_limit() {
  World w;
  w.bringUp();
  const uint32_t pause[5] = {1000, 2000, 4000, 8000, 16000};
  for (int k = 0; k < 5; k++) {
    uint32_t back = busOffAndBack(w);
    // not before the pause (PANEL_HB keep coming), then 3 PANEL_HB (≤ 300 ms more)
    TEST_ASSERT_TRUE_MESSAGE(back >= pause[k], "switched back before the pause");
    TEST_ASSERT_TRUE_MESSAGE(back <= pause[k] + 400, "too late after the pause");
    TEST_ASSERT_EQUAL(k + 1, w.mode->busOffRun());
  }
  uint32_t held = busOffAndBack(w);  // the 6th: held
  TEST_ASSERT_TRUE(held >= panel_mode::kHoldMs);
  TEST_ASSERT_TRUE(held <= panel_mode::kHoldMs + 400);
  TEST_ASSERT_EQUAL(6, w.mode->busOffRun());
  TEST_ASSERT_EQUAL(6, int(w.mode->counters().busOffs));
  // a minute of normal mode without bus-off: the count starts over
  w.run(int(panel_mode::kCleanMs) + 10);
  TEST_ASSERT_EQUAL(0, w.mode->busOffRun());
  uint32_t again = busOffAndBack(w);
  TEST_ASSERT_TRUE(again >= 1000 && again <= 1400);
}

static void test_l4_single_shot_in_flight_at_bus_off_is_bus_error() {
  World w;
  w.hbOn = false;
  // 3 PANEL_HB by hand (no automatic single-shot results), then the first ANNOUNCE
  for (int i = 0; i < 700 && !w.port.single; i++) {
    if (i % 100 == 0) w.feed(w.panelHb(w.panelGen));
    w.now++;
    w.mode->tick();
  }
  TEST_ASSERT_TRUE(w.port.single);
  w.port.off = true;
  w.now++;
  w.mode->tick();
  TEST_ASSERT_TRUE(w.mode->listening());
  TEST_ASSERT_EQUAL(1, int(w.mode->counters().busOffs));
}

static void test_d2_enter_exit_and_overrun_voids_anchor() {
  World w;
  w.bringUp();
  w.port.overruns = 3;  // the driver lost frames: the next frames have no usable anchor
  w.feed(vehFast(2500, 1, 2, 1));
  w.run(5);
  TEST_ASSERT_TRUE(w.mode->counters().overrunVoids >= 1);
  TEST_ASSERT_EQUAL_MESSAGE(0, w.mode->view(w.now).fresh & panel_core::kFreshRpm, "no valid without an anchor");
  w.mode->exit();
  TEST_ASSERT_FALSE(w.mode->active());
  VehicleView v = w.mode->view(w.now);
  TEST_ASSERT_EQUAL(0u, v.lastUpdate);
  TEST_ASSERT_EQUAL(0, v.fresh);
}

// ---- D3: each reader, fresh / stale / no data --------------------------------------------------------

static bool existing3s(uint32_t lastUpdate, uint32_t now) { return lastUpdate && now - lastUpdate <= 3000; }

static void test_d3_readers_fresh_stale_none() {
  World w;
  w.bringUp();
  // no data
  VehicleView v = w.mode->view(w.now);
  bool d3 = existing3s(v.lastUpdate, w.now);
  TEST_ASSERT_FALSE(vrules::fieldFresh(true, v.fresh, d3, 0));
  TEST_ASSERT_EQUAL_FLOAT(0, vrules::vehicleScale(0, vrules::fieldFresh(true, v.fresh, d3, 0), v.rpm, v.speed, v.throttle, 200, 1000, 7000));
  TEST_ASSERT_FALSE(vrules::tempKnown(true, v.fresh));
  TEST_ASSERT_TRUE_MESSAGE(vrules::parkedCut(true, 2, d3 && v.rpm > 300), "no data: parked, power cut (3 s rule)");
  // fresh
  uint8_t seqF = 1, seqS = 1;
  for (int i = 0; i < 10; i++) {
    w.feed(vehFast(6000, 7, 2, seqF++, 1000, 50));
    w.feed(vehSlow(130, 1, 2, seqS++));
    w.run(20);
  }
  v = w.mode->view(w.now);
  d3 = existing3s(v.lastUpdate, w.now);
  bool rpmF = vrules::fieldFresh(true, v.fresh, d3, 0);
  TEST_ASSERT_TRUE(rpmF);
  TEST_ASSERT_EQUAL_FLOAT(5000.f / 6000.f, vrules::vehicleScale(0, rpmF, v.rpm, v.speed, v.throttle, 200, 1000, 7000));
  TEST_ASSERT_EQUAL_FLOAT(0.5f, vrules::vehicleScale(1, vrules::fieldFresh(true, v.fresh, d3, 1), v.rpm, v.speed, v.throttle, 200, 1000, 7000));
  TEST_ASSERT_EQUAL_FLOAT(0.5f, vrules::vehicleScale(2, vrules::fieldFresh(true, v.fresh, d3, 2), v.rpm, v.speed, v.throttle, 200, 1000, 7000));
  TEST_ASSERT_TRUE(vrules::shiftLight(rpmF, v.rpm, 5500, 7000));
  TEST_ASSERT_TRUE(vrules::tempKnown(true, v.fresh));
  TEST_ASSERT_EQUAL(90, v.temp);
  TEST_ASSERT_FALSE(vrules::parkedCut(true, 2, d3 && v.rpm > 300));
  // stale (500 ms without VEH): rpm keeps its value for the 3 s rule, but scales and shift light are off
  w.run(500);
  v = w.mode->view(w.now);
  d3 = existing3s(v.lastUpdate, w.now);
  rpmF = vrules::fieldFresh(true, v.fresh, d3, 0);
  TEST_ASSERT_TRUE_MESSAGE(d3 && v.rpm > 300, "engineRunning by the 3 s rule still");
  TEST_ASSERT_FALSE(rpmF);
  TEST_ASSERT_EQUAL_FLOAT_MESSAGE(0, vrules::vehicleScale(0, rpmF, v.rpm, v.speed, v.throttle, 200, 1000, 7000), "rpm scale off on stale rpm");
  TEST_ASSERT_FALSE_MESSAGE(vrules::shiftLight(rpmF, v.rpm, 5500, 7000), "shift light off on stale rpm");
  TEST_ASSERT_FALSE_MESSAGE(vrules::tempKnown(true, v.fresh), "a stale temperature is not read (not 0 °C)");
  TEST_ASSERT_FALSE(vrules::parkedCut(true, 2, d3 && v.rpm > 300));
  // 3 s later: the existing rule ends engineRunning → cutPowerEngineOff parks
  w.run(2600);
  v = w.mode->view(w.now);
  d3 = existing3s(v.lastUpdate, w.now);
  TEST_ASSERT_FALSE(d3);
  TEST_ASSERT_TRUE(vrules::parkedCut(true, 2, d3 && v.rpm > 300));
}

static void test_d3_other_modes_readers_unchanged() {
  // modes 0…3: every reader uses the one 3 s stamp as before, temperature always readable
  for (uint8_t mask : {uint8_t(0), uint8_t(15)}) {
    for (bool d3 : {false, true}) {
      for (int field = 0; field < 3; field++) TEST_ASSERT_EQUAL(d3, vrules::fieldFresh(false, mask, d3, field));
      TEST_ASSERT_TRUE(vrules::tempKnown(false, mask));
      // the old scaleFraction / shiftLightActive expressions
      float oldRpm = d3 ? float(4000 - 1000) / 6000.f : 0;
      TEST_ASSERT_EQUAL_FLOAT(oldRpm, vrules::vehicleScale(0, d3, 4000, 100, 40, 200, 1000, 7000));
      TEST_ASSERT_EQUAL_FLOAT(d3 ? 0.5f : 0, vrules::vehicleScale(1, d3, 4000, 100, 40, 200, 1000, 7000));
      TEST_ASSERT_EQUAL_FLOAT(d3 ? 0.4f : 0, vrules::vehicleScale(2, d3, 4000, 100, 40, 200, 1000, 7000));
      TEST_ASSERT_EQUAL(d3, vrules::shiftLight(d3, 6000, 5500, 7000));
    }
  }
  TEST_ASSERT_EQUAL_FLOAT(0, vrules::vehicleScale(0, true, 500, 0, 0, 0, 1000, 7000));  // below minRPM: 0 as max(0, …)
  TEST_ASSERT_EQUAL_FLOAT(0, vrules::vehicleScale(1, true, 0, 50, 0, 0, 1000, 7000));   // maxSpeed 0
  TEST_ASSERT_EQUAL_FLOAT(0, vrules::vehicleScale(0, true, 3000, 0, 0, 0, 7000, 7000)); // span 0
  for (bool cut : {false, true})
    for (int off = 0; off <= 2; off++)
      for (bool run : {false, true}) TEST_ASSERT_EQUAL(cut && off == 2 && !run, vrules::parkedCut(cut, off, run));
}


// ---- A5 rev 2 / R93: a bus with acknowledgements, panel + L8 nodes ----------------------------------

// One L8 on the bus: the firmware's PanelMode over a model port, the real dispatcher.
struct L8Node {
  ModelPort port;
  ModelStore store;
  uint32_t* now;
  uint32_t rng;
  panel_mode::PanelMode* mode;
  L8Node(uint32_t* clock, uint8_t macLast, uint32_t seed) : now(clock), rng(seed) {
    panel_mode::Host h;
    h.ctx = this;
    h.now = [](void* c) { return *static_cast<L8Node*>(c)->now; };
    h.dispatch = [](void*, const uint8_t*, size_t, uint8_t*, size_t) -> size_t { return 0; };
    h.rand = xs;
    h.randCtx = &rng;
    mode = new panel_mode::PanelMode(port, store, h);
    uint8_t mac[6] = {0x10, 0xBD, 0xA3, 0x5F, 0x51, macLast};
    uint8_t boot[16];
    for (int i = 0; i < 16; i++) boot[i] = uint8_t(macLast + i);
    mode->enter(mac, 1, boot, "L8-X", "test", 3);
  }
  ~L8Node() { delete mode; }
};

// The bus: a frame is acknowledged only if a node other than the sender is in normal mode.
// The panel (MCU) is always normal. Unacknowledged, the panel's controller is error-active
// (TEC < 128: listeners get nothing, TEC += 8 per attempt) and then error-passive (listeners
// get the frame and each retransmission; TEC no longer grows): A5 rev 2.
struct AckBus {
  uint32_t now = 1000;
  std::vector<L8Node*> l8;
  bool panelOn = true, foreignAcker = false, listenersAck = false;  // the last: a mutant of the model
  int tec = 0, maxTec = 0;
  uint32_t unacked = 0, acked = 0, copies = 0, cancelled = 0, l8Frames = 0, firstCopyAfter = 0;
  uint32_t panelGen = 0x55667788;
  int ackers(const L8Node* except) const {
    int n = foreignAcker ? 1 : 0;
    for (auto* x : l8)
      if (x != except && (listenersAck || !x->port.listenOnly)) n++;
    return n;
  }
  CanFrame hb() const {
    CanFrame f;
    f.id = panel_rx::kIdPanelHb;
    uint8_t d[8] = {0xCA, 0xFE, 0x00, 0x37, uint8_t(panelGen >> 24), uint8_t(panelGen >> 16), uint8_t(panelGen >> 8), uint8_t(panelGen)};
    memcpy(f.data, d, 8);
    return f;
  }
  void panelSends(const CanFrame& f) {
    if (ackers(nullptr) > 0) {
      for (auto* x : l8) x->port.rxq.push_back(f);
      if (tec) tec--;
      acked++;
      return;
    }
    // ≈ 7 attempts per ms for 5 ms, then the MCU cancels (kHpCancelMs)
    for (int a = 0; a < 35; a++) {
      unacked++;
      if (tec < 128) {
        tec += 8;
      } else if (copies < 100000) {
        if (!copies) firstCopyAfter = unacked;
        copies++;
        if (a % 7 == 0)  // listeners see every retransmission; a few per frame keep the queue small
          for (auto* x : l8) x->port.rxq.push_back(f);
      }
      maxTec = tec > maxTec ? tec : maxTec;
    }
    cancelled++;
  }
  void step() {
    now++;
    if (panelOn && now % 100 == 0) panelSends(hb());
    for (auto* x : l8) {
      x->mode->tick();
      auto out = x->port.sent;
      x->port.sent.clear();
      for (auto& [f, single] : out) {
        l8Frames++;  // the panel (normal) acknowledges it
        if (single) x->port.singleResultReady = true;
        for (auto* y : l8)
          if (y != x) y->port.rxq.push_back(f);
      }
    }
  }
};

static void test_b1_panel_and_two_listening_l8_come_up() {
  AckBus b;
  L8Node a(&b.now, 0x01, 11), c(&b.now, 0x02, 12);
  b.l8 = {&a, &c};
  uint32_t t0 = b.now, upA = 0, upC = 0;
  for (int ms = 0; ms < 3000; ms++) {
    b.step();
    if (!upA && !a.mode->listening()) upA = b.now - t0;
    if (!upC && !c.mode->listening()) upC = b.now - t0;
  }
  char m[200];
  snprintf(m, sizeof m, "B1: L8 normal after %u / %u ms; panel frames unacknowledged %u (cancelled %u), first copy to listeners after %u attempts, max TEC %d; L8 frames %u",
           unsigned(upA), unsigned(upC), unsigned(b.unacked), unsigned(b.cancelled), unsigned(b.firstCopyAfter), b.maxTec, unsigned(b.l8Frames));
  TEST_MESSAGE(m);
  printf("%s\n", m);
  TEST_ASSERT_TRUE_MESSAGE(upA && upC, "both L8 left listen-only");
  TEST_ASSERT_TRUE_MESSAGE(b.firstCopyAfter >= 17, "listeners got nothing while the panel was error-active");
  TEST_ASSERT_TRUE_MESSAGE(b.maxTec < 256, "the panel never goes bus-off on acknowledgement errors");
  TEST_ASSERT_TRUE_MESSAGE(upA >= 200 && upC >= 200, "three periods, not three retransmissions");
  TEST_ASSERT_TRUE(b.acked > 0);     // once an L8 is normal it acknowledges the panel
  TEST_ASSERT_TRUE(b.l8Frames > 0);  // and its ANNOUNCE is acknowledged by the panel
}

static void test_b2_two_l8_without_panel_60_s_send_nothing() {
  AckBus b;
  b.panelOn = false;
  L8Node a(&b.now, 0x01, 11), c(&b.now, 0x02, 12);
  b.l8 = {&a, &c};
  for (int ms = 0; ms < 60000; ms++) b.step();
  TEST_ASSERT_EQUAL(0, a.port.sentCount + c.port.sentCount);
  TEST_ASSERT_FALSE(a.port.everNormal || c.port.everNormal);
}

static void test_b3_foreign_acknowledging_node_without_panel() {
  AckBus b;
  b.panelOn = false;
  b.foreignAcker = true;
  L8Node a(&b.now, 0x01, 11), c(&b.now, 0x02, 12);
  b.l8 = {&a, &c};
  uint32_t r = 5;
  for (int ms = 0; ms < 20000; ms++) {
    if (ms % 10 == 0) {  // the foreign node's own traffic (not PANEL_HB)
      CanFrame f;
      f.extended = xs(&r) & 1;
      f.id = f.extended ? (xs(&r) & 0x1FFFFFFF) : (xs(&r) & 0x7FF);
      if (f.id == panel_rx::kIdPanelHb) f.id ^= 1;
      for (auto* x : b.l8) x->port.rxq.push_back(f);
    }
    b.step();
  }
  TEST_ASSERT_TRUE(a.mode->listening() && c.mode->listening());
  TEST_ASSERT_EQUAL(0, a.port.sentCount + c.port.sentCount);
}

static void test_b4_retransmissions_are_not_periods() {
  World w;
  w.hbOn = false;
  for (int k = 0; k < 2; k++) {
    for (int d = 0; d < 5; d++) w.feed(w.panelHb(w.panelGen));  // one frame, retransmitted
    w.run(1);
    TEST_ASSERT_EQUAL(k + 1, w.mode->hbCount());
    w.run(99);
  }
  for (int d = 0; d < 5; d++) w.feed(w.panelHb(w.panelGen));
  w.run(1);
  TEST_ASSERT_FALSE(w.mode->listening());  // the third period
  // 40 ms apart is a retransmission too: ignored, not a reset
  World v;
  v.hbOn = false;
  v.feed(v.panelHb(v.panelGen));
  v.run(40);
  v.feed(v.panelHb(v.panelGen));
  v.run(1);
  TEST_ASSERT_EQUAL(1, v.mode->hbCount());
}

static void test_r94_bitrate_change_does_not_restart_the_driver_in_panel_mode() {
  for (int mode : {0, 1, 2, 3, 5}) TEST_ASSERT_TRUE(vmode::bitrateChangeRestarts(mode));
  TEST_ASSERT_FALSE_MESSAGE(vmode::bitrateChangeRestarts(4), "panel mode: only PanelMode switches TWAI");
  // in normal state the port stays normal: nothing but PanelMode called setListenOnly
  World w;
  w.bringUp();
  int toListen = w.port.toListen;
  for (int sel = 0; sel <= 5; sel++)
    if (vmode::bitrateChangeRestarts(4)) w.port.setListenOnly(vmode::plan(4).listenOnly);  // what vehicleSetBitrate would do
  w.run(50);
  TEST_ASSERT_EQUAL(toListen, w.port.toListen);
  TEST_ASSERT_FALSE(w.mode->listening());
  TEST_ASSERT_FALSE(w.port.listenOnly);
  TEST_ASSERT_EQUAL_STRING("ready", w.mode->phase());
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_d1_old_modes_plan_unchanged_panel_off);
  RUN_TEST(test_d2_discovery_address_and_stored_address);
  RUN_TEST(test_d2_request_through_the_real_dispatcher);
  RUN_TEST(test_d2_veh_per_field_freshness_and_status_a_rpm_fresh);
  RUN_TEST(test_l1_no_panel_hb_60_s_nothing_sent_never_normal);
  RUN_TEST(test_l2_three_panel_hb_then_normal_and_discovery);
  RUN_TEST(test_l3_panel_hb_lost_back_to_listen);
  RUN_TEST(test_l4_bus_off_listen_backoff_and_limit);
  RUN_TEST(test_l4_single_shot_in_flight_at_bus_off_is_bus_error);
  RUN_TEST(test_b1_panel_and_two_listening_l8_come_up);
  RUN_TEST(test_b2_two_l8_without_panel_60_s_send_nothing);
  RUN_TEST(test_b3_foreign_acknowledging_node_without_panel);
  RUN_TEST(test_b4_retransmissions_are_not_periods);
  RUN_TEST(test_r94_bitrate_change_does_not_restart_the_driver_in_panel_mode);
  RUN_TEST(test_d2_enter_exit_and_overrun_voids_anchor);
  RUN_TEST(test_d3_readers_fresh_stale_none);
  RUN_TEST(test_d3_other_modes_readers_unchanged);
  return UNITY_END();
}
