// L8 receive side of the panel frames (v6 §7, §14.3, §14.4, §15). Portable, no heap, no
// exceptions; time is the caller's u32 ms, compared modulo 2^32 (§1). Not in the l8_v3
// image yet. Decisions and criteria: README.md.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace panel_rx {

constexpr uint32_t kIdVehFast = 0x04103F00;
constexpr uint32_t kIdVehSlow = 0x04203F00;
constexpr uint32_t kIdPanelHb = 0x08403F00;
constexpr uint32_t kIdPanelState = 0x08503F00;

// §14.3: 5.2 ms bus + H_max 2 ms + margin. H_max is an assumption, checked on the bench only.
constexpr uint32_t kHmaxMs = 2;
constexpr uint32_t kMarginMs = 10;
constexpr int32_t kMaxQueueMs = 50;    // t_read - anchor above this: valid not used
constexpr int32_t kFreshMs = 300;      // visible freshness (§14.4)
constexpr int32_t kRunMs = 3000;       // engineRunning / vehicleDataFresh by rpm
constexpr uint16_t kRunRpm = 300;
constexpr int32_t kSeqSilenceMs = 1000;  // no VEH of a type this long: seq forgotten
constexpr int32_t kForgetMs = 60000;     // fields older than this are dropped (keeps i32 sane)

struct Frame {
  uint32_t id = 0;
  bool ext = false;
  uint8_t dlc = 0;
  uint8_t data[8] = {};
};

enum class Kind : uint8_t { VehFast, VehSlow, PanelHb, PanelState, NotOurs, Bad };
enum class BadWhy : uint8_t { None, StdId, Dlc, Header };

// §15 software filter: extended ID, DLC 8, the exact panel IDs. A frame with a panel
// function but another prio/src/dst/tid is Bad/Header; other functions are NotOurs.
Kind classify(const Frame& f, BadWhy* why);

struct VehFast {
  uint16_t rpm, speed;
  uint8_t throttle, valid, age, seq;
};
struct VehSlow {
  uint8_t coolant, oil, gear, ign, valid, age, seq;
};
struct PanelHb {
  uint32_t panelId, panelGen;
};
struct PanelState {
  uint32_t mcuGen;
  uint8_t flags, seq, vehAgeMax;
  bool gatewayAlive() const { return flags & 1; }
  bool vehLinkOk() const { return flags & 2; }
};

VehFast parseVehFast(const uint8_t d[8]);
VehSlow parseVehSlow(const uint8_t d[8]);
PanelHb parsePanelHb(const uint8_t d[8]);
PanelState parsePanelState(const uint8_t d[8]);

// ---- §14.3 queue stamp ("software queue -> task") --------------------------------------
//
// One loop iteration is poll() then read(). A Poll can only come from poll(), and read()
// takes the anchor from the Poll it is given: a frame uses the stamp taken before its read,
// a later stamp cannot reach it. snapshot() takes t0 from the clock before it asks the
// queue, so "stamp before the emptiness check" is in the interface, not in the caller.

struct Anchor {
  bool has = false;
  uint32_t t = 0;
};

class Poll {
 public:
  Anchor anchor() const { return a_; }

 private:
  friend class QueueStamp;
  Poll() = default;
  Anchor a_;
};

struct Stamp {
  bool usable = false;  // the frame's valid bits may be used
  uint32_t anchor = 0;  // meaningful if usable
  uint32_t tRead = 0;
};

// The input of a poll step. Made only by take() (clock first, then queue) or, in tests,
// by at() with explicit values.
class Snapshot {
 public:
  template <class Clock, class Queue>
  static Snapshot take(Clock& clock, Queue& q) {
    Snapshot s;
    s.t0 = clock();
    s.empty = q.rxQueueEmpty();
    s.overrun = q.overrunGrew();
    return s;
  }
  static Snapshot at(uint32_t t0, bool queueEmptyAfterT0, bool overrunGrew) {
    Snapshot s;
    s.t0 = t0;
    s.empty = queueEmptyAfterT0;
    s.overrun = overrunGrew;
    return s;
  }

 private:
  friend class QueueStamp;
  Snapshot() = default;
  uint32_t t0 = 0;
  bool empty = false;
  bool overrun = false;
};

class QueueStamp {
 public:
  Poll poll(const Snapshot& s);
  Stamp read(const Poll& p, uint32_t tRead, bool overrunGrew);

  // A driver loop step with the order fixed: snapshot, receive, then t_read.
  template <class Clock, class Queue>
  bool step(Clock& clock, Queue& q, Frame* f, Stamp* st) {
    Poll p = poll(Snapshot::take(clock, q));
    if (!q.receive(f)) return false;
    uint32_t tRead = clock();
    *st = read(p, tRead, q.overrunGrew());
    return true;
  }
  Anchor anchor() const { return a_; }

 private:
  Anchor a_;
};

// ---- §14.4 consumer -----------------------------------------------------------------

enum Field : uint8_t { kRpm, kSpeed, kThrottle, kCoolant, kOil, kGear, kIgn, kFields };

struct FieldVal {
  bool has = false;
  uint16_t value = 0;
  uint32_t m = 0;  // L8 time of measurement, never later than the true one (under H)
};

struct Counters {
  uint32_t veh_fast = 0, veh_slow = 0, panel_hb = 0, panel_state = 0;
  uint32_t not_ours = 0, bad_std_id = 0, bad_dlc = 0, bad_header = 0;
  uint32_t seq_dropped = 0;
  uint32_t stamp_unusable = 0;   // frame had valid bits but the stamp was not usable
  uint32_t age_unbounded = 0;    // valid bits with age 0xFE
  uint32_t age_missing = 0;      // valid bits with age 0xFF
  uint32_t value_invalid = 0;    // a valid bit on a VEH_SLOW value 0xFF ("none")
  uint32_t seq_reset_gen = 0, seq_reset_silence = 0;
  uint32_t fields_updated = 0;
};

enum class Result : uint8_t { Accepted, NotOurs, Bad, SeqDropped };

class Consumer {
 public:
  // Every frame from the queue, with the Stamp of its read.
  Result onFrame(const Frame& f, const Stamp& s);
  // Forget fields older than kForgetMs; call at least once per 2^31 ms (the loop does).
  void expire(uint32_t now);

  const FieldVal& field(Field i) const { return f_[i]; }
  // now - m; -1 if no value
  int32_t ageMs(Field i, uint32_t now) const;
  bool fresh(Field i, uint32_t now) const;  // §14.4 visible freshness, 300 ms
  bool vehicleDataFresh(uint32_t now) const;
  bool engineRunning(uint32_t now) const;

  bool hasHb() const { return hasHb_; }
  const PanelHb& hb() const { return hb_; }
  uint32_t hbAt() const { return hbAt_; }
  bool hasState() const { return hasState_; }
  const PanelState& state() const { return state_; }
  uint32_t stateAt() const { return stateAt_; }
  const Counters& counters() const { return c_; }

 private:
  struct Seq {
    bool has = false;
    uint8_t last = 0;
    uint32_t at = 0;
  };
  bool seqAccept(Seq& q, uint8_t seq, uint32_t t);
  void applyFields(uint8_t valid, uint8_t age, const Stamp& s, Field first, int n, const uint16_t* v);

  FieldVal f_[kFields];
  Seq fast_, slow_;
  bool hasHb_ = false, hasState_ = false;
  PanelHb hb_{};
  PanelState state_{};
  uint32_t hbAt_ = 0, stateAt_ = 0;
  Counters c_;
};

}  // namespace panel_rx
