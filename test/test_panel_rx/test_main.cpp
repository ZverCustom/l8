// lib/panel_rx (task 026): §7 parsing, §15 filter, §14.3 queue stamp and seq, §14.4
// consumer, the §14.5 rows that need the L8, the freshness property and bad input.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unity.h>

#include <map>
#include <random>
#include <string>
#include <vector>

#include "panel_rx.h"

using namespace panel_rx;

void setUp() {}
void tearDown() {}

static Frame mk(uint32_t id, std::vector<uint8_t> d, bool ext = true) {
  Frame f;
  f.id = id;
  f.ext = ext;
  f.dlc = static_cast<uint8_t>(d.size());
  for (size_t i = 0; i < d.size() && i < 8; i++) f.data[i] = d[i];
  return f;
}

static Frame fast(uint16_t rpm, uint16_t speed, uint8_t thr, uint8_t valid, uint8_t age, uint8_t seq) {
  return mk(kIdVehFast, {static_cast<uint8_t>(rpm >> 8), static_cast<uint8_t>(rpm), static_cast<uint8_t>(speed >> 8),
                         static_cast<uint8_t>(speed), thr, valid, age, seq});
}
static Frame slow(uint8_t c, uint8_t o, uint8_t g, uint8_t i, uint8_t valid, uint8_t age, uint8_t seq) {
  return mk(kIdVehSlow, {c, o, g, i, valid, age, seq, 0});
}
static Frame state(uint32_t gen, uint8_t flags, uint8_t seq = 0, uint8_t vam = 0xFF) {
  return mk(kIdPanelState, {static_cast<uint8_t>(gen >> 24), static_cast<uint8_t>(gen >> 16), static_cast<uint8_t>(gen >> 8),
                            static_cast<uint8_t>(gen), flags, seq, vam, 0});
}
static Frame hb(uint32_t id, uint32_t gen) {
  return mk(kIdPanelHb, {static_cast<uint8_t>(id >> 24), static_cast<uint8_t>(id >> 16), static_cast<uint8_t>(id >> 8),
                         static_cast<uint8_t>(id), static_cast<uint8_t>(gen >> 24), static_cast<uint8_t>(gen >> 16),
                         static_cast<uint8_t>(gen >> 8), static_cast<uint8_t>(gen)});
}

// A usable stamp: queue was empty at `anchor`, frame read at `tRead`.
static Stamp at(uint32_t anchor, uint32_t tRead) {
  QueueStamp q;
  Poll p = q.poll(Snapshot::at(anchor, true, false));
  return q.read(p, tRead, false);
}

// ---- L1: §7 bytes and the 021 vectors ---------------------------------------------------

static void test_l1_section7_example() {
  Frame f = mk(0x04103F00, {0x09, 0xC4, 0x02, 0x0B, 0x11, 0x07, 0x08, 0x2A});
  BadWhy w;
  TEST_ASSERT_EQUAL(static_cast<int>(Kind::VehFast), static_cast<int>(classify(f, &w)));
  VehFast v = parseVehFast(f.data);
  TEST_ASSERT_EQUAL(2500, v.rpm);
  TEST_ASSERT_EQUAL(523, v.speed);
  TEST_ASSERT_EQUAL(17, v.throttle);
  TEST_ASSERT_EQUAL(7, v.valid);
  TEST_ASSERT_EQUAL(8, v.age);
  TEST_ASSERT_EQUAL(0x2A, v.seq);
  // the field time: anchor - 8*4 - 10
  Consumer c;
  c.onFrame(f, at(1000, 1001));
  TEST_ASSERT_EQUAL_UINT32(1000 - 32 - 10, c.field(kRpm).m);
  TEST_ASSERT_EQUAL_UINT32(1000 - 32 - 10, c.field(kSpeed).m);
  TEST_ASSERT_EQUAL(523, c.field(kSpeed).value);
}

static void test_l1_temperatures() {
  // §14.5: -20 -> 0x14, 0 -> 0x28, 100 -> 0x8C, 120 -> 0xA0, none 0xFF
  const uint8_t enc[] = {0x14, 0x28, 0x8C, 0xA0};
  const int c[] = {-20, 0, 100, 120};
  for (int i = 0; i < 4; i++) {
    VehSlow v = parseVehSlow(slow(enc[i], 0xFF, 0xFF, 0xFF, 1, 1, 0).data);
    TEST_ASSERT_EQUAL(c[i], static_cast<int>(v.coolant) - 40);
  }
  Consumer k;
  k.onFrame(slow(0xFF, 0x28, 0xFF, 0xFF, 0x03, 2, 1), at(500, 500));  // coolant "none" with valid
  TEST_ASSERT_FALSE(k.field(kCoolant).has);
  TEST_ASSERT_TRUE(k.field(kOil).has);
  TEST_ASSERT_EQUAL(1, k.counters().value_invalid);
}

static uint8_t ageByte(const std::vector<int>& a) {
  if (a.empty()) return 0xFF;
  int m = 0;
  for (int x : a) m = x > m ? x : m;
  if (m >= 1016) return 0xFE;
  return static_cast<uint8_t>((m + 3) / 4);
}

static void test_l1_vectors_of_021() {
  const char* path = getenv("PANEL_RX_VECTORS");
  FILE* fp = fopen(path ? path : "test/test_panel_rx/veh_vectors.txt", "r");
  TEST_ASSERT_NOT_NULL_MESSAGE(fp, "vector file");
  struct B {
    std::string kind;
    int seq = -1;
    std::map<int, std::pair<long, int>> fields;
    unsigned long p[3] = {0, 0, 0};
    unsigned long hb[2] = {0, 0};
    uint32_t id = 0;
    uint8_t d[8] = {};
  };
  std::vector<B> blocks;
  char line[512];
  B cur;
  while (fgets(line, sizeof line, fp)) {
    char k[32];
    if (line[0] == '#' || sscanf(line, "%31s", k) != 1) continue;
    std::string key = k;
    if (key == "frame") {
      cur = B();
      char kind[32];
      sscanf(line, "frame %31s", kind);
      cur.kind = kind;
    } else if (key == "seq") {
      sscanf(line, "seq %d", &cur.seq);
    } else if (key == "field") {
      int i, a;
      long v;
      sscanf(line, "field %d %ld %d", &i, &v, &a);
      cur.fields[i] = {v, a};
    } else if (key == "panel") {
      sscanf(line, "panel %lu %lu %lu", &cur.p[0], &cur.p[1], &cur.p[2]);
    } else if (key == "hb") {
      sscanf(line, "hb %lu %lu", &cur.hb[0], &cur.hb[1]);
    } else if (key == "id") {
      unsigned long id;
      sscanf(line, "id %lx", &id);
      cur.id = static_cast<uint32_t>(id);
    } else if (key == "data") {
      char hex[32];
      sscanf(line, "data %31s", hex);
      for (int i = 0; i < 8; i++) {
        unsigned b;
        sscanf(hex + 2 * i, "%2x", &b);
        cur.d[i] = static_cast<uint8_t>(b);
      }
      blocks.push_back(cur);
    }
  }
  fclose(fp);
  TEST_ASSERT_EQUAL(62, static_cast<int>(blocks.size()));
  int n[6] = {0, 0, 0, 0, 0, 0};
  for (const B& b : blocks) {
    Frame f;
    f.id = b.id;
    f.ext = true;
    f.dlc = 8;
    memcpy(f.data, b.d, 8);
    BadWhy w;
    Kind k = classify(f, &w);
    std::vector<int> ages;
    uint8_t valid = 0;
    for (auto& [i, va] : b.fields) ages.push_back(va.second);
    if (b.kind == "VEH_FAST") {
      TEST_ASSERT_EQUAL(static_cast<int>(Kind::VehFast), static_cast<int>(k));
      VehFast v = parseVehFast(f.data);
      for (auto& [i, va] : b.fields) valid |= static_cast<uint8_t>(1 << i);
      TEST_ASSERT_EQUAL(valid, v.valid);
      TEST_ASSERT_EQUAL(ageByte(ages), v.age);
      TEST_ASSERT_EQUAL(b.seq, v.seq);
      long vals[3] = {v.rpm, v.speed, v.throttle};
      for (auto& [i, va] : b.fields) TEST_ASSERT_EQUAL(va.first, vals[i]);
      n[0]++;
    } else if (b.kind == "VEH_SLOW") {
      TEST_ASSERT_EQUAL(static_cast<int>(Kind::VehSlow), static_cast<int>(k));
      VehSlow v = parseVehSlow(f.data);
      for (auto& [i, va] : b.fields) valid |= static_cast<uint8_t>(1 << (i - 3));
      TEST_ASSERT_EQUAL(valid, v.valid);
      TEST_ASSERT_EQUAL(ageByte(ages), v.age);
      TEST_ASSERT_EQUAL(b.seq, v.seq);
      long vals[4] = {v.coolant, v.oil, v.gear, v.ign};
      for (int i = 0; i < 4; i++) {
        auto it = b.fields.find(3 + i);
        TEST_ASSERT_EQUAL(it == b.fields.end() ? 0xFF : it->second.first, vals[i]);
      }
      n[1]++;
    } else if (b.kind == "PANEL_STATE") {
      TEST_ASSERT_EQUAL(static_cast<int>(Kind::PanelState), static_cast<int>(k));
      PanelState s = parsePanelState(f.data);
      TEST_ASSERT_EQUAL_UINT32(b.p[0], s.mcuGen);
      TEST_ASSERT_EQUAL(b.p[1] != 0, s.gatewayAlive());
      TEST_ASSERT_EQUAL(b.p[2] != 0, s.vehLinkOk());
      TEST_ASSERT_EQUAL(b.seq, s.seq);
      TEST_ASSERT_EQUAL(ageByte(ages), s.vehAgeMax);
      n[2]++;
    } else if (b.kind == "PANEL_HB") {
      TEST_ASSERT_EQUAL(static_cast<int>(Kind::PanelHb), static_cast<int>(k));
      PanelHb h = parsePanelHb(f.data);
      TEST_ASSERT_EQUAL_UINT32(b.hb[0], h.panelId);
      TEST_ASSERT_EQUAL_UINT32(b.hb[1], h.panelGen);
      n[3]++;
    } else {
      TEST_ASSERT_EQUAL(static_cast<int>(Kind::NotOurs), static_cast<int>(k));
      n[4]++;
    }
  }
  printf("vectors: VEH_FAST %d, VEH_SLOW %d, PANEL_STATE %d, PANEL_HB %d, not ours %d\n", n[0], n[1], n[2], n[3], n[4]);
  TEST_ASSERT_EQUAL(26, n[0]);
  TEST_ASSERT_EQUAL(24, n[1]);
  TEST_ASSERT_EQUAL(3, n[2]);
  TEST_ASSERT_EQUAL(1, n[3]);
  TEST_ASSERT_EQUAL(8, n[4]);
}

// ---- L2: filter -----------------------------------------------------------------------

static void test_l2_filter() {
  Consumer c;
  Stamp s = at(0, 0);
  Frame ok = fast(1000, 0, 0, 1, 1, 1);
  Frame stdid = ok;
  stdid.ext = false;
  stdid.id = 0x0F0;
  Frame dlc7 = ok;
  dlc7.dlc = 7;
  Frame prio = ok;
  prio.id = kIdVehFast ^ (1u << 27);  // prio 3
  Frame src = ok;
  src.id = kIdVehFast | (0x05u << 14);
  Frame dst = ok;
  dst.id = (kIdVehFast & ~(0x3Fu << 8)) | (0x01u << 8);
  Frame tid = ok;
  tid.id = kIdVehFast | 0x01;
  Frame hiBits = ok;
  hiBits.id = kIdVehFast | 0x20000000;
  Frame other = mk(0x11040100, {0, 0, 0, 0, 0, 0, 0, 0});  // XDATA
  TEST_ASSERT_EQUAL(static_cast<int>(Result::Bad), static_cast<int>(c.onFrame(stdid, s)));
  TEST_ASSERT_EQUAL(static_cast<int>(Result::Bad), static_cast<int>(c.onFrame(dlc7, s)));
  for (const Frame* f : {&prio, &src, &dst, &tid, &hiBits})
    TEST_ASSERT_EQUAL(static_cast<int>(Result::Bad), static_cast<int>(c.onFrame(*f, s)));
  TEST_ASSERT_EQUAL(static_cast<int>(Result::NotOurs), static_cast<int>(c.onFrame(other, s)));
  TEST_ASSERT_FALSE(c.field(kRpm).has);
  TEST_ASSERT_EQUAL(1, c.counters().bad_std_id);
  TEST_ASSERT_EQUAL(1, c.counters().bad_dlc);
  TEST_ASSERT_EQUAL(5, c.counters().bad_header);
  TEST_ASSERT_EQUAL(1, c.counters().not_ours);
  TEST_ASSERT_EQUAL(0, c.counters().veh_fast);
  TEST_ASSERT_EQUAL(static_cast<int>(Result::Accepted), static_cast<int>(c.onFrame(ok, s)));
  TEST_ASSERT_TRUE(c.field(kRpm).has);
}

// ---- L3: the queue stamp, §14.5 rows ---------------------------------------------------

static void test_l3_norm_estimate_not_younger() {
  // §14.5 norm: measured at L8 time 1000-12 (true age 12 at bus end), MCU wrote age 3 (12 ms);
  // queue empty at t0 = 1000, frame read at 1001.
  QueueStamp q;
  Poll p = q.poll(Snapshot::at(1000, true, false));
  Stamp s = q.read(p, 1001, false);
  TEST_ASSERT_TRUE(s.usable);
  Consumer c;
  c.onFrame(fast(2500, 0, 0, 1, 3, 1), s);
  uint32_t trueMeas = 1000 - 12;
  TEST_ASSERT_TRUE(static_cast<int32_t>(trueMeas - c.field(kRpm).m) >= 0);
  TEST_ASSERT_EQUAL(22, c.ageMs(kRpm, 1000));
  TEST_ASSERT_TRUE(c.fresh(kRpm, 1001));
}

static void test_l3_frame_500ms_in_the_l8_queue() {
  // anchor 5 (emptiness snapshot), the task reads at 510: 505 > 50 -> valid ignored
  QueueStamp q;
  Consumer c;
  c.onFrame(fast(900, 0, 0, 1, 1, 1), q.read(q.poll(Snapshot::at(0, true, false)), 1, false));
  uint32_t m0 = c.field(kRpm).m;
  Poll p = q.poll(Snapshot::at(5, true, false));
  Stamp s = q.read(p, 510, false);
  TEST_ASSERT_FALSE(s.usable);
  c.onFrame(fast(3000, 0, 0, 1, 3, 2), s);
  TEST_ASSERT_EQUAL(900, c.field(kRpm).value);
  TEST_ASSERT_EQUAL_UINT32(m0, c.field(kRpm).m);
  TEST_ASSERT_EQUAL(1, c.counters().stamp_unusable);
  TEST_ASSERT_FALSE(c.fresh(kRpm, 510 + 300));
}

static void test_l3_tail_of_an_old_burst_after_a_2s_pause() {
  // 100 frames waiting; every iteration's snapshot sees a non-empty queue until the last one
  // is read; then the next snapshot is empty and only the next new frame gets that anchor.
  QueueStamp q;
  Consumer c;
  Poll p0 = q.poll(Snapshot::at(1000, true, false));
  (void)p0;
  uint32_t t = 3000;
  for (int i = 0; i < 100; i++) {
    Poll p = q.poll(Snapshot::at(t, false, false));  // not empty: anchor stays 1000
    TEST_ASSERT_EQUAL_UINT32(1000, p.anchor().t);
    Stamp s = q.read(p, t + 1, false);
    TEST_ASSERT_FALSE(s.usable);
    c.onFrame(fast(static_cast<uint16_t>(1000 + i), 0, 0, 1, 1, static_cast<uint8_t>(i + 1)), s);
    t += 1;
  }
  TEST_ASSERT_FALSE(c.field(kRpm).has);
  // the last old frame was read with the old token; now the queue is empty at t
  Poll p = q.poll(Snapshot::at(t, true, false));
  Stamp s = q.read(p, t + 15, false);  // the next new frame
  TEST_ASSERT_TRUE(s.usable);
  TEST_ASSERT_EQUAL_UINT32(t, s.anchor);
  c.onFrame(fast(4242, 0, 0, 1, 1, 101), s);
  TEST_ASSERT_EQUAL(4242, c.field(kRpm).value);
  TEST_ASSERT_EQUAL(100, c.counters().stamp_unusable);
}

static void test_l3_emptiness_snapshot_race() {
  // t0 = 100 is taken before the check; the frame came at 101; the task stood 100..600
  struct Clock {
    uint32_t t;
    uint32_t operator()() { return t; }
  } clock{100};
  struct Queue {
    Clock* clock;
    bool rxQueueEmpty() {
      clock->t = 101;  // the check itself happens after t0
      return true;
    }
    bool overrunGrew() { return false; }
    bool receive(Frame* f) {
      clock->t = 601;  // the task stood
      *f = fast(5000, 0, 0, 1, 1, 1);
      return true;
    }
  } queue{&clock};
  QueueStamp q;
  Frame f;
  Stamp s;
  TEST_ASSERT_TRUE(q.step(clock, queue, &f, &s));
  TEST_ASSERT_EQUAL_UINT32(601, s.tRead);
  TEST_ASSERT_EQUAL_UINT32(100, q.anchor().t);
  TEST_ASSERT_FALSE(s.usable);  // 601 - 100 = 501 > 50; no false age of 11 ms
  Consumer c;
  c.onFrame(f, s);
  TEST_ASSERT_FALSE(c.field(kRpm).has);
}

static void test_l3_fifo_overrun() {
  QueueStamp q;
  Poll p = q.poll(Snapshot::at(100, true, false));
  Stamp s = q.read(p, 101, true);  // overrun seen at read
  TEST_ASSERT_FALSE(s.usable);
  TEST_ASSERT_FALSE(q.anchor().has);
  // until the next empty snapshot: no anchor
  p = q.poll(Snapshot::at(102, false, false));
  TEST_ASSERT_FALSE(q.read(p, 103, false).usable);
  // overrun at the poll clears too, even with an empty queue
  p = q.poll(Snapshot::at(104, true, true));
  TEST_ASSERT_FALSE(q.read(p, 105, false).usable);
  p = q.poll(Snapshot::at(106, true, false));
  Stamp ok = q.read(p, 107, false);
  TEST_ASSERT_TRUE(ok.usable);
  TEST_ASSERT_EQUAL_UINT32(106, ok.anchor);
  // before the first snapshot ever: nothing usable
  QueueStamp fresh;
  TEST_ASSERT_FALSE(fresh.read(fresh.poll(Snapshot::at(1, false, false)), 2, false).usable);
}

static void test_l3_a_later_stamp_does_not_reach_an_earlier_token() {
  QueueStamp q;
  Poll old = q.poll(Snapshot::at(0, true, false));
  Poll now = q.poll(Snapshot::at(1000, true, false));
  TEST_ASSERT_FALSE(q.read(old, 1001, false).usable);  // the token keeps its own anchor (0)
  TEST_ASSERT_TRUE(q.read(now, 1001, false).usable);
}

// ---- L4: fields -----------------------------------------------------------------------

static void test_l4_mixed_valid_speed_does_not_extend_rpm() {
  Consumer c;
  c.onFrame(fast(2000, 100, 10, 0x07, 2, 1), at(0, 0));  // all fresh at m = -18
  // later: rpm without valid, speed valid A_now 20 -> age 5
  c.onFrame(fast(0, 200, 0, 0x02, 5, 2), at(2500, 2500));
  TEST_ASSERT_EQUAL(2000, c.field(kRpm).value);
  TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(-18), c.field(kRpm).m);
  TEST_ASSERT_EQUAL_UINT32(2500 - 30, c.field(kSpeed).m);
  TEST_ASSERT_EQUAL(200, c.field(kSpeed).value);
  TEST_ASSERT_TRUE(c.engineRunning(2900));    // 2918 ms
  TEST_ASSERT_FALSE(c.engineRunning(3000));   // 3018 ms
  TEST_ASSERT_FALSE(c.vehicleDataFresh(3000));
  TEST_ASSERT_TRUE(c.fresh(kSpeed, 2770));  // 300 ms
  TEST_ASSERT_FALSE(c.fresh(kSpeed, 2771));
  // rpm A_now 0, speed A_now 200 -> age 50: both m = anchor - 210
  c.onFrame(fast(1500, 300, 0, 0x03, 50, 3), at(4000, 4000));
  TEST_ASSERT_EQUAL_UINT32(4000 - 210, c.field(kRpm).m);
  TEST_ASSERT_EQUAL_UINT32(4000 - 210, c.field(kSpeed).m);
}

static void test_l4_ecu_freeze() {
  // rpm valid, then the ECU freezes: valid rpm = 0 from now on; engineRunning goes false by 3 s
  Consumer c;
  c.onFrame(fast(2000, 0, 0, 1, 1, 1), at(0, 0));
  uint8_t seq = 2;
  for (uint32_t t = 20; t <= 4000; t += 20) c.onFrame(fast(2000, 50, 0, 0x02, 1, seq++), at(t, t));
  TEST_ASSERT_TRUE(c.fresh(kSpeed, 4000));
  TEST_ASSERT_FALSE(c.engineRunning(4000));
  TEST_ASSERT_FALSE(c.fresh(kRpm, 4000));
  TEST_ASSERT_TRUE(c.engineRunning(2980));
}

static void test_l4_dashcore_freeze() {
  // the MCU keeps sending with valid 0 (no VEH_SET for 200 ms) and gatewayAlive = 0
  Consumer c;
  c.onFrame(state(9, 0x03), at(0, 0));
  c.onFrame(fast(2000, 0, 0, 1, 1, 1), at(0, 0));
  uint8_t seq = 2;
  for (uint32_t t = 20; t <= 3200; t += 20) {
    c.onFrame(fast(0, 0, 0, 0, 0xFF, seq++), at(t, t));
    if (t % 100 == 0) c.onFrame(state(9, t >= 300 ? 0 : 0x01), at(t, t));
  }
  TEST_ASSERT_FALSE(c.fresh(kRpm, 320));
  TEST_ASSERT_FALSE(c.engineRunning(3200));
  TEST_ASSERT_FALSE(c.state().gatewayAlive());
  TEST_ASSERT_FALSE(c.state().vehLinkOk());
}

static void test_l4_mcu_restart() {
  Consumer c;
  c.onFrame(state(7, 0x03), at(0, 0));
  c.onFrame(fast(2000, 0, 0, 1, 1, 100), at(0, 0));
  // new MCU: its VEH start at seq 0 without valid; before its PANEL_STATE they are dropped
  TEST_ASSERT_EQUAL(static_cast<int>(Result::SeqDropped), static_cast<int>(c.onFrame(fast(0, 0, 0, 0, 0xFF, 0), at(40, 40))));
  c.onFrame(state(8, 0x00), at(60, 60));
  TEST_ASSERT_EQUAL(1, c.counters().seq_reset_gen);
  TEST_ASSERT_EQUAL(static_cast<int>(Result::Accepted), static_cast<int>(c.onFrame(fast(0, 0, 0, 0, 0xFF, 1), at(80, 80))));
  TEST_ASSERT_EQUAL(2000, c.field(kRpm).value);  // no valid from the new MCU yet: nothing updated
  TEST_ASSERT_FALSE(c.fresh(kRpm, 400));
  // once §14.2 lets packets through, data resume
  c.onFrame(fast(1200, 0, 0, 1, 2, 2), at(500, 500));
  TEST_ASSERT_TRUE(c.engineRunning(500));
  TEST_ASSERT_EQUAL(1200, c.field(kRpm).value);
}

static void test_l4_panel_hb_and_state_exposed() {
  Consumer c;
  c.onFrame(hb(0xCAFEBABE, 11), at(10, 10));
  c.onFrame(state(77, 0x03, 5, 50), at(20, 21));
  TEST_ASSERT_TRUE(c.hasHb());
  TEST_ASSERT_EQUAL_UINT32(0xCAFEBABE, c.hb().panelId);
  TEST_ASSERT_EQUAL_UINT32(11, c.hb().panelGen);
  TEST_ASSERT_EQUAL_UINT32(10, c.hbAt());
  TEST_ASSERT_EQUAL_UINT32(77, c.state().mcuGen);
  TEST_ASSERT_TRUE(c.state().gatewayAlive());
  TEST_ASSERT_TRUE(c.state().vehLinkOk());
  TEST_ASSERT_EQUAL(50, c.state().vehAgeMax);
  TEST_ASSERT_EQUAL_UINT32(21, c.stateAt());
  // reserved flag bits ignored
  c.onFrame(state(77, 0xF8), at(30, 30));
  TEST_ASSERT_EQUAL(0, c.state().flags);
}

static void test_l4_unbounded_or_missing_age_updates_nothing() {
  Consumer c;
  c.onFrame(fast(2000, 0, 0, 1, 0xFE, 1), at(0, 0));
  c.onFrame(fast(2000, 0, 0, 1, 0xFF, 2), at(0, 0));
  c.onFrame(slow(0x50, 0, 0, 1, 0x0F, 0xFE, 1), at(0, 0));
  TEST_ASSERT_FALSE(c.field(kRpm).has);
  TEST_ASSERT_FALSE(c.field(kCoolant).has);
  TEST_ASSERT_EQUAL(2, c.counters().age_unbounded);
  TEST_ASSERT_EQUAL(1, c.counters().age_missing);
  TEST_ASSERT_FALSE(c.engineRunning(0));
}

static void test_l4_clock_wrap_and_expire() {
  Consumer c;
  uint32_t t = 0xFFFFFFF0u;
  c.onFrame(fast(2000, 0, 0, 1, 1, 1), at(t, t));
  TEST_ASSERT_TRUE(c.engineRunning(t + 100));  // across 2^32
  TEST_ASSERT_EQUAL(114, c.ageMs(kRpm, t + 100));
  TEST_ASSERT_FALSE(c.engineRunning(t + 3100));
  c.expire(t + 70000);
  TEST_ASSERT_FALSE(c.field(kRpm).has);
  TEST_ASSERT_FALSE(c.engineRunning(t + 0x80000000u));  // never fresh again by wrap
}

// ---- L5: seq --------------------------------------------------------------------------

static void test_l5_seq() {
  Consumer c;
  auto f = [&](uint8_t seq, uint32_t t) { return c.onFrame(fast(1000, 0, 0, 1, 1, seq), at(t, t)); };
  auto acc = static_cast<int>(Result::Accepted), drop = static_cast<int>(Result::SeqDropped);
  TEST_ASSERT_EQUAL(acc, static_cast<int>(f(254, 0)));
  TEST_ASSERT_EQUAL(acc, static_cast<int>(f(255, 20)));
  TEST_ASSERT_EQUAL(acc, static_cast<int>(f(0, 40)));
  TEST_ASSERT_EQUAL(acc, static_cast<int>(f(1, 60)));
  TEST_ASSERT_EQUAL(drop, static_cast<int>(f(253, 80)));
  TEST_ASSERT_EQUAL(drop, static_cast<int>(f(1, 90)));  // same
  TEST_ASSERT_EQUAL(acc, static_cast<int>(f(128, 100)));  // +127
  TEST_ASSERT_EQUAL(drop, static_cast<int>(f(0, 120)));   // +128
  // FAST and SLOW apart
  TEST_ASSERT_EQUAL(acc, static_cast<int>(c.onFrame(slow(0x50, 0, 0, 1, 1, 1, 5), at(130, 130))));
  TEST_ASSERT_EQUAL(acc, static_cast<int>(c.onFrame(slow(0x50, 0, 0, 1, 1, 1, 6), at(140, 140))));
  TEST_ASSERT_EQUAL(drop, static_cast<int>(f(100, 150)));
  // 1 s silence of the type: forgotten
  TEST_ASSERT_EQUAL(acc, static_cast<int>(f(0, 100 + 1001)));
  TEST_ASSERT_EQUAL(1, c.counters().seq_reset_silence);
  // panelGen and mcuGen
  c.onFrame(hb(1, 11), at(1200, 1200));
  c.onFrame(state(5, 3), at(1200, 1200));
  TEST_ASSERT_EQUAL(drop, static_cast<int>(f(200, 1210)));
  c.onFrame(hb(1, 12), at(1220, 1220));
  TEST_ASSERT_EQUAL(acc, static_cast<int>(f(200, 1230)));
  TEST_ASSERT_EQUAL(drop, static_cast<int>(f(150, 1240)));
  c.onFrame(state(6, 3), at(1250, 1250));
  TEST_ASSERT_EQUAL(acc, static_cast<int>(f(50, 1260)));
  TEST_ASSERT_EQUAL(2, c.counters().seq_reset_gen);
  // the same generation again does not reset
  c.onFrame(state(6, 3), at(1270, 1270));
  TEST_ASSERT_EQUAL(drop, static_cast<int>(f(40, 1280)));
}

// ---- L6: freshness property ----------------------------------------------------------

struct SimResult {
  long frames = 0, updated = 0, lost = 0, violations = 0, late_updates = 0;
  long maxMarginUs = 0, minMarginUs = 1L << 40;
};

// Model time in µs (64 bit); the L8 clock is (offset + t/1000) mod 2^32 ms.
static SimResult simulate(uint32_t seed, double hMaxMs, int iters) {
  std::mt19937 rng(seed);
  auto U = [&](double a, double b) { return std::uniform_real_distribution<double>(a, b)(rng); };
  auto P = [&](double p) { return U(0, 1) < p; };
  const int64_t offsetMs = static_cast<int64_t>(0xFFFFFFFFu) - 30000 + (seed % 1000);
  auto clock = [&](int64_t us) { return static_cast<uint32_t>(offsetMs + us / 1000); };
  struct F {
    int64_t enter, meas;
    Frame f;
  };
  // frames produced by the MCU every 20 ms; measurements at the source, A_now conservative
  std::vector<F> frames;
  int64_t t = 1000000;
  uint8_t seq = 0;
  for (int i = 0; i < iters; i++) {
    t += static_cast<int64_t>(U(18000, 22000));
    if (P(0.01)) t += static_cast<int64_t>(U(0, 300000));  // MCU / bus gaps
    int64_t trueAge = static_cast<int64_t>(U(1000, 199000));
    int64_t meas = t - trueAge;
    int64_t aNow = trueAge + static_cast<int64_t>(U(0, 20000)) + 1000;  // over-estimate + A2.8
    uint8_t age = static_cast<uint8_t>((aNow + 3999) / 4000);
    int64_t busEnd = t + static_cast<int64_t>(U(0, 5200));
    int64_t enter = busEnd + static_cast<int64_t>(U(0, hMaxMs * 1000));
    if (P(0.02)) {  // a burst: several frames enter back to back
      enter = frames.empty() ? enter : frames.back().enter + static_cast<int64_t>(U(0, 200));
      if (enter < busEnd) enter = busEnd;
    }
    uint16_t rpm = static_cast<uint16_t>(i);
    frames.push_back({enter, meas, fast(rpm, 0, 0, 1, age, ++seq)});
  }
  std::stable_sort(frames.begin(), frames.end(), [](const F& a, const F& b) { return a.enter < b.enter; });
  const size_t cap = 32;  // software queue length; beyond it frames are lost and counted
  SimResult r;
  r.frames = static_cast<long>(frames.size());
  QueueStamp q;
  Consumer c;
  size_t next = 0;  // first unread
  size_t lostSeen = 0, lostCount = 0;
  int64_t now = 1000000;
  std::vector<bool> lost(frames.size(), false);
  auto settleLoss = [&](int64_t at) {
    // frames that found the queue full by `at` are lost
    size_t inQ = 0;
    for (size_t i = next; i < frames.size() && frames[i].enter <= at; i++) {
      if (lost[i]) continue;
      if (inQ >= cap) {
        lost[i] = true;
        lostCount++;
      } else {
        inQ++;
      }
    }
  };
  auto skipLost = [&]() {
    while (next < frames.size() && lost[next]) next++;
  };
  while (next < frames.size()) {
    if (P(0.002)) now += static_cast<int64_t>(U(0, 2000000));  // task pause up to 2 s
    int64_t t0 = now;
    int64_t check = t0 + static_cast<int64_t>(U(0, 300));
    if (P(0.003)) check += static_cast<int64_t>(U(0, 600000));  // preempted between t0 and check
    settleLoss(check);
    skipLost();
    bool empty = next >= frames.size() || frames[next].enter > check;
    bool ovr = lostCount != lostSeen;
    lostSeen = lostCount;
    Poll p = q.poll(Snapshot::at(clock(t0), empty, ovr));
    int64_t recv = check + static_cast<int64_t>(U(0, 300));
    if (P(0.003)) recv += static_cast<int64_t>(U(0, 600000));
    settleLoss(recv);
    skipLost();
    if (next >= frames.size()) break;
    int64_t got;
    if (frames[next].enter <= recv) {
      got = recv;
    } else if (frames[next].enter <= recv + 10000) {
      got = frames[next].enter;
    } else {
      now = recv + 10000;
      continue;
    }
    const F& fr = frames[next];
    next++;
    int64_t tReadUs = got + static_cast<int64_t>(U(0, 200));
    if (P(0.01)) tReadUs += static_cast<int64_t>(U(0, 100000));
    settleLoss(tReadUs);
    bool ovr2 = lostCount != lostSeen;
    lostSeen = lostCount;
    Stamp s = q.read(p, clock(tReadUs), ovr2);
    uint32_t before = c.counters().fields_updated;
    c.onFrame(fr.f, s);
    if (c.counters().fields_updated != before) {
      r.updated++;
      // m (L8 ms) unwrapped against the read time
      int64_t tReadMs = offsetMs + tReadUs / 1000;
      int64_t mMs = tReadMs - static_cast<int32_t>(clock(tReadUs) - c.field(kRpm).m);
      int64_t measL8Us = offsetMs * 1000 + fr.meas;
      int64_t margin = measL8Us - mMs * 1000;  // >= 0: age never under-estimated
      if (margin < 0) r.violations++;
      if (margin < r.minMarginUs) r.minMarginUs = margin;
      if (margin > r.maxMarginUs) r.maxMarginUs = margin;
      if (static_cast<int32_t>(s.tRead - s.anchor) > kMaxQueueMs || tReadUs - fr.enter > (kMaxQueueMs + 1) * 1000)
        r.late_updates++;
    }
    now = tReadUs;
  }
  r.lost = static_cast<long>(lostCount);
  return r;
}

static void test_l6_freshness_property() {
  long frames = 0, updated = 0, lost = 0, minMargin = 1L << 40;
  for (uint32_t seed = 1; seed <= 200; seed++) {
    SimResult r = simulate(seed, kHmaxMs, 3000);
    TEST_ASSERT_EQUAL_MESSAGE(0, r.violations, "age under-estimated");
    TEST_ASSERT_EQUAL_MESSAGE(0, r.late_updates, "field updated from a late frame");
    frames += r.frames;
    updated += r.updated;
    lost += r.lost;
    if (r.minMarginUs < minMargin) minMargin = r.minMarginUs;
  }
  printf("L6: H_max %u ms, 200 seeds, %ld frames, %ld updated, %ld lost in queue, min margin %ld us\n", kHmaxMs, frames,
         updated, lost, minMargin);
  TEST_ASSERT_TRUE(updated > frames / 2);
  TEST_ASSERT_TRUE(lost > 0);
}

static void test_l6_model_has_teeth_when_h_is_violated() {
  // the same model with H up to 20 ms (assumption broken): the property fails somewhere
  long violations = 0;
  for (uint32_t seed = 1; seed <= 50; seed++) violations += simulate(seed, 20.0, 3000).violations;
  printf("L6 negative control: H up to 20 ms -> %ld under-estimates\n", violations);
  TEST_ASSERT_TRUE(violations > 0);
}

// ---- L8: bad input ----------------------------------------------------------------------

static void test_l8_bad_input_never_gives_valid() {
  std::mt19937 rng(26);
  Consumer c;
  QueueStamp q;
  for (int i = 0; i < 200000; i++) {
    Frame f;
    f.ext = rng() % 8 != 0;
    uint32_t pick = rng() % 6;
    const uint32_t ids[] = {kIdVehFast, kIdVehSlow, kIdPanelHb, kIdPanelState};
    f.id = pick < 4 ? ids[pick] ^ (rng() % 3 == 0 ? (1u << (rng() % 29)) : 0) : rng() & 0x3FFFFFFF;
    f.dlc = static_cast<uint8_t>(rng() % 3 == 0 ? rng() % 16 : 8);
    for (auto& b : f.data) b = static_cast<uint8_t>(rng());
    uint32_t t = rng();
    Poll p = q.poll(Snapshot::at(t, rng() % 2, rng() % 10 == 0));
    Stamp s = q.read(p, t + rng() % 100, rng() % 10 == 0);
    uint32_t before = c.counters().fields_updated;
    Result r = c.onFrame(f, s);
    bool updated = c.counters().fields_updated != before;
    if (updated) {
      BadWhy w;
      Kind k = classify(f, &w);
      TEST_ASSERT_TRUE(r == Result::Accepted);
      TEST_ASSERT_TRUE(k == Kind::VehFast || k == Kind::VehSlow);
      TEST_ASSERT_TRUE(f.ext && f.dlc == 8);
      TEST_ASSERT_TRUE(s.usable);
      uint8_t age = k == Kind::VehFast ? f.data[6] : f.data[5];
      TEST_ASSERT_TRUE(age < 0xFE);
    }
  }
  const Counters& k = c.counters();
  printf("L8: fast %u slow %u hb %u state %u not_ours %u std %u dlc %u header %u seq %u unusable %u updated %u\n",
         k.veh_fast, k.veh_slow, k.panel_hb, k.panel_state, k.not_ours, k.bad_std_id, k.bad_dlc, k.bad_header,
         k.seq_dropped, k.stamp_unusable, k.fields_updated);
}

static void test_l9_sizes() {
  printf("sizes: Consumer %zu, QueueStamp %zu, Poll %zu, Stamp %zu, Frame %zu bytes\n", sizeof(Consumer),
         sizeof(QueueStamp), sizeof(Poll), sizeof(Stamp), sizeof(Frame));
  TEST_ASSERT_TRUE(sizeof(Consumer) <= 256);
  TEST_ASSERT_TRUE(sizeof(QueueStamp) <= 16);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_l1_section7_example);
  RUN_TEST(test_l1_temperatures);
  RUN_TEST(test_l1_vectors_of_021);
  RUN_TEST(test_l2_filter);
  RUN_TEST(test_l3_norm_estimate_not_younger);
  RUN_TEST(test_l3_frame_500ms_in_the_l8_queue);
  RUN_TEST(test_l3_tail_of_an_old_burst_after_a_2s_pause);
  RUN_TEST(test_l3_emptiness_snapshot_race);
  RUN_TEST(test_l3_fifo_overrun);
  RUN_TEST(test_l3_a_later_stamp_does_not_reach_an_earlier_token);
  RUN_TEST(test_l4_mixed_valid_speed_does_not_extend_rpm);
  RUN_TEST(test_l4_ecu_freeze);
  RUN_TEST(test_l4_dashcore_freeze);
  RUN_TEST(test_l4_mcu_restart);
  RUN_TEST(test_l4_panel_hb_and_state_exposed);
  RUN_TEST(test_l4_unbounded_or_missing_age_updates_nothing);
  RUN_TEST(test_l4_clock_wrap_and_expire);
  RUN_TEST(test_l5_seq);
  RUN_TEST(test_l6_freshness_property);
  RUN_TEST(test_l6_model_has_teeth_when_h_is_violated);
  RUN_TEST(test_l8_bad_input_never_gives_valid);
  RUN_TEST(test_l9_sizes);
  return UNITY_END();
}
