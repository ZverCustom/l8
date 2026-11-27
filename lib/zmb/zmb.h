// ZMB v1 transport core (contract v6 §5-§6) for the L8 firmware, portable C++17.
// Sans-IO: CAN frames in/out, time as wrapping uint32 milliseconds. No Arduino, TWAI or
// FreeRTOS dependency and no allocation: the receiver assembles into a buffer given by
// the caller (PSRAM later) and the sender reads the caller's message in place.
// Not part of the firmware CAN path yet (next U3b steps: tid manager, budget, adapter).
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace zmb {

constexpr size_t kHeaderLen = 20;
constexpr size_t kMinMessage = kHeaderLen;
constexpr size_t kMaxMessage = 24596;
constexpr uint32_t kNBsMs = 150, kTAckMs = 150, kNCrMs = 100;
constexpr uint8_t kMaxWaits = 3;

namespace func { constexpr uint8_t kFlow = 0x08, kAck = 0x09, kRabort = 0x0A, kXstart = 0x10, kXdata = 0x11, kXabort = 0x12; }
namespace flow { constexpr uint8_t kCts = 0, kWait = 1, kOverflow = 2; }
namespace ack { constexpr uint8_t kOk = 0, kCrcFail = 1, kTooLarge = 2, kNoBuffer = 3, kGenMismatch = 4, kBadHeader = 5; }
namespace reason { constexpr uint8_t kCancel = 1, kTimeout = 2, kSeqGap = 3, kOverflow = 4, kRestart = 5, kSuperseded = 6; }
enum class MsgType : uint8_t { Request = 1, Response = 2, Hello = 3, HelloAck = 4 };

struct CanFrame {
  uint32_t id = 0;
  bool extended = true;
  bool rtr = false;
  uint8_t dlc = 8;
  uint8_t data[8] = {0};
};

struct AddrId {
  uint8_t prio, func, src, dst, tid8;
  uint32_t encode() const;
  static AddrId decode(uint32_t id);
};

enum class Kind : uint8_t { None, XStart, XData, XAbort, Flow, Ack, RAbort };

// One parsed transport frame (fields valid for its kind).
struct TFrame {
  Kind kind = Kind::None;
  uint16_t len = 0, tid = 0, seq = 0, expected = 0;
  uint32_t crc = 0;
  uint8_t status = 0, block = 0, stmin = 0, reason = 0;
  uint8_t bytes[6] = {0};
};

// Builds the frame `t` from src to dst (reserved bytes zero).
CanFrame build(uint8_t src, uint8_t dst, uint8_t tid8, const TFrame& t);
// Parses a ZMB transport frame; false for standard/remote/short/out-of-range/unknown.
bool parse(const CanFrame& f, AddrId& a, TFrame& t);

TFrame xstart(uint16_t len, uint16_t tid, uint32_t crc);
TFrame xdata(uint16_t seq, const uint8_t* six);
TFrame flowFrame(uint16_t tid, uint8_t status, uint8_t block, uint8_t stmin, uint16_t expected);
TFrame ackFrame(uint16_t tid, uint8_t status, uint32_t crc);
TFrame abortFrame(Kind k, uint16_t tid, uint8_t reason);

uint32_t crc32(const uint8_t* data, size_t len);

struct Header {
  uint8_t ver = 1, mtype = 0;
  uint16_t tid = 0, reqTid = 0;
  uint32_t panelGen = 0, bootTag = 0, reqCrc = 0;
  void encode(uint8_t* out20) const;
  static bool decode(const uint8_t* in, size_t len, Header& h);
};

// HELLO payload (§6.2). Strings are views into the decoded buffer (no copies).
struct Hello {
  uint8_t devType = 1;
  const char* deviceId = nullptr;
  uint8_t deviceIdLen = 0;
  uint8_t bootId[16] = {0};
  const char* firmware = nullptr;
  uint8_t firmwareLen = 0;
  uint8_t protoMajor = 1, protoMinor = 0;
  uint32_t caps = 0;
  uint16_t maxMessage = 0;
  uint8_t hwRev = 0;
  // Writes the payload; returns its length or 0 if it does not fit.
  size_t encode(uint8_t* out, size_t cap) const;
  static bool decode(const uint8_t* in, size_t len, Hello& h);
};

struct HelloAck {
  uint8_t status = 0, addr = 0, d10Proto = 1;
  uint32_t panelId = 0;
  size_t encode(uint8_t* out7) const;
  static bool decode(const uint8_t* in, size_t len, HelloAck& h);
};

bool deviceIdHasMac(const char* id, size_t len, const uint8_t mac[6]);
bool expired(uint32_t deadline, uint32_t now);

// ---- sender (§5.3) ----
enum class TxState : uint8_t { Idle, Starting, WaitFlow, Sending, WaitAck };
enum class TxResult : uint8_t { None, NotDelivered, Rejected, Delivered, Unknown };
struct TxOutcome {
  TxResult result = TxResult::None;
  uint8_t code = 0;  // reason (NotDelivered) or ACK status (Rejected)
};

// Frames are offered (poll) and become real only when the lower layer confirms the write
// (confirm). The confirmed write of the last XDATA is "committed" (§12). The caller keeps
// `msg` alive until the outcome.
class Sender {
 public:
  Sender(uint8_t local, uint8_t peer) : local_(local), peer_(peer) {}
  bool start(const uint8_t* msg, size_t len, uint16_t tid);
  bool poll(uint32_t now, CanFrame& out) const;
  void confirm(uint32_t now);
  void refuse() {}
  TxOutcome onFrame(const CanFrame& f, uint32_t now);
  TxOutcome tick(uint32_t now);
  TxOutcome cancel(uint8_t why);
  TxOutcome generationChanged() { return cancel(reason::kRestart); }
  TxState state() const { return state_; }
  bool committed() const { return state_ == TxState::WaitAck; }
  uint16_t tid() const { return tid_; }
  uint32_t crc() const { return crc_; }

 private:
  enum class Cand : uint8_t { None, XStart, XData, XAbort };
  Cand candidate(uint32_t now) const;
  TxOutcome finish(TxResult r, uint8_t code, uint8_t abortReason);
  uint8_t local_, peer_;
  const uint8_t* msg_ = nullptr;
  size_t len_ = 0;
  uint16_t tid_ = 0, nextSeq_ = 1, lastSeq_ = 0, blockLeft_ = 0;
  uint32_t crc_ = 0, lastXdataAt_ = 0, deadline_ = 0;
  bool haveLastXdata_ = false, haveDeadline_ = false;
  uint8_t stmin_ = 0, waits_ = 0, pendingAbort_ = 0;
  TxState state_ = TxState::Idle;
};

// ---- receiver (§5.4) ----
enum class Side : uint8_t { Panel, Module };
struct RxContext {
  uint8_t local = 0;
  Side side = Side::Module;
  uint32_t panelGen = 0;
  uint32_t ownBootTag = 0;      // module
  bool havePeerBootTag = false;  // panel
  uint32_t peerBootTag = 0;
  bool haveAssignedMac = false;  // panel
  uint8_t assignedMac[6] = {0};
  bool srcAssigned = true;
};

enum class RxKind : uint8_t { None, Message, Rejected, Aborted, TimedOut, Duplicate };
struct RxEvent {
  RxKind kind = RxKind::None;
  uint16_t tid = 0;
  uint8_t code = 0;  // ACK status / abort reason
  size_t len = 0;    // Message: bytes in the buffer
  Header header;
};
struct RxOutput {
  bool hasReply = false;
  CanFrame reply;
  RxEvent event;
};

// One bounded assembly for one sender. On completion the length, CRC and whole header
// are checked before the message is handed out; the ACK comes in the same output and
// must be queued before the handler runs.
class Receiver {
 public:
  Receiver(uint8_t peer, uint8_t* buf, size_t cap, uint8_t block = 16, uint8_t stmin = 0)
      : peer_(peer), buf_(buf), cap_(cap < kMaxMessage ? cap : kMaxMessage), block_(block ? block : 1), stmin_(stmin) {}
  RxOutput onFrame(const CanFrame& f, const RxContext& ctx, uint32_t now);
  RxEvent tick(uint32_t now);
  const uint8_t* message() const { return buf_; }
  bool busy() const { return active_; }

 private:
  uint8_t check(const RxContext& ctx, Header& h) const;
  CanFrame reply(const RxContext& ctx, uint16_t tid, const TFrame& t) const;
  uint8_t peer_;
  uint8_t* buf_;
  size_t cap_;
  uint8_t block_, stmin_;
  bool active_ = false, haveLast_ = false;
  uint16_t tid_ = 0, len_ = 0, expected_ = 0, last_ = 0;
  uint32_t crc_ = 0, deadline_ = 0;
  uint8_t sinceFlow_ = 0;
  uint16_t lastTid_ = 0, lastLen_ = 0;
  uint32_t lastCrc_ = 0;
};

// Reply matching (§5.5) for replies without a JSON id (HELLO_ACK on the module side).
// RESPONSE/JSON-id matching belongs to the panel (Rust).
bool matchReply(const Header& h, uint16_t tid, uint32_t crc, uint32_t panelGen, uint32_t bootTag);

}  // namespace zmb
