// L8 panel mode core (v6 §4.2, §5–§6, §12, §14.3–14.4, §15): one parser for the bus frame
// stream, discovery, ZMB transport, panel data, and the device's command layer behind a
// function pointer. Sans-IO, no heap, time is the caller's u32 ms. README.md.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "panel_rx.h"
#include "zmb.h"
#include "zmb_disc.h"

namespace panel_core {

struct CoreIo {
  void* ctx = nullptr;
  // A frame to transmit now; singleShot: ANNOUNCE/BEACON, answer with singleShotResult().
  void (*tx)(void* ctx, const zmb::CanFrame& f, bool singleShot) = nullptr;
  // The device's command layer: request JSON -> response JSON in resp (cap bytes).
  // Returns the response length, 0 for none.
  size_t (*dispatch)(void* ctx, const uint8_t* req, size_t len, uint8_t* resp, size_t cap) = nullptr;
  // STATUS_A contents (§7).
  void (*statusA)(void* ctx, uint8_t* flags, uint16_t* rev, uint16_t* saved, uint8_t* chOn, uint8_t* chFault) = nullptr;
  void (*log)(void* ctx, const char* text) = nullptr;
};

struct Identity {
  uint8_t mac[6] = {};
  uint8_t devType = 1;
  uint8_t bootId[16] = {};
  const char* deviceId = nullptr;  // "L8-" + MAC
  const char* firmware = nullptr;
  uint8_t hwRev = 3;
  uint32_t storedPanelId = 0;
  uint8_t storedAddr = 0;
};

struct Buffers {
  uint8_t* rx;  // receive assembly, >= zmb::kMaxMessage
  size_t rxCap;
  uint8_t* resp;  // response message (20-byte header + JSON), >= kHeaderLen + reply
  size_t respCap;
  zmb::TidEntry* tids;
  size_t tidCap;
};

struct Counters {
  uint32_t panel = 0, discovery = 0, transport = 0;
  uint32_t drop_std = 0, drop_rtr = 0, drop_dlc = 0, drop_not_ours = 0, drop_other_dst = 0, drop_other_src = 0;
  uint32_t drop_unassigned = 0;  // transport frame for us before the address is confirmed
  uint32_t requests = 0, dispatched = 0, dup_request = 0, busy_dropped = 0, not_ready = 0;
  uint32_t responses_sent = 0, boundaries = 0, hello_timeouts = 0;
};

enum class SingleShot : uint8_t { Sent, Lost, BusError };

// The vehicle fields as the existing VehicleData (vehicle.h) and the existing rules read them.
struct VehicleView {
  uint16_t rpm = 0;
  uint8_t speed = 0;  // km/h
  uint8_t throttle = 0;
  int8_t temp = 0;  // coolant °C
  uint32_t lastUpdate = 0;  // m of rpm (L8 ms), 0 = no data
};

class PanelCore {
 public:
  PanelCore(const Identity& id, const Buffers& b, const CoreIo& io, zmb::RandFn rand, void* randCtx);

  void onFrame(const zmb::CanFrame& f, const panel_rx::Stamp& s, uint32_t now);
  void tick(uint32_t now);
  void singleShotResult(SingleShot r, uint32_t now);

  bool ready() const { return phase_ == Phase::Ready; }
  uint8_t addr() const { return disc_.addr(); }
  const zmb::ModuleDiscovery& discovery() const { return disc_; }
  const panel_rx::Consumer& panel() const { return rxPanel_; }
  const Counters& counters() const { return c_; }
  uint32_t bootTag() const { return bootTag_; }

  VehicleView panelVehicle(uint32_t now) const;

 private:
  enum class Phase : uint8_t { Unassigned, Hello, Ready };
  void pump(uint32_t now);
  void boundary(uint32_t now);
  bool startMsg(zmb::MsgType t, uint8_t* msg, size_t len, uint32_t now);
  void sendHello(uint32_t now);
  void onMessage(const zmb::RxEvent& e, uint32_t now);
  void outcome(const zmb::TxOutcome& o, uint32_t now);
  void transport(const zmb::CanFrame& f, uint32_t now);
  void emit(const zmb::CanFrame& f, bool single);
  void log(const char* s) const;
  zmb::RxContext ctx() const;

  Identity id_;
  Buffers b_;
  CoreIo io_;
  uint32_t bootTag_ = 0;
  zmb::ModuleDiscovery disc_;
  panel_rx::Consumer rxPanel_;
  zmb::TidTable tids_;
  zmb::Sender tx_{0, 0};
  zmb::Receiver rx_{0, nullptr, 0};
  bool haveTransport_ = false;
  Phase phase_ = Phase::Unassigned;
  uint32_t lastGen_ = 0, tidGen_ = 0;
  bool haveTidGen_ = false;
  zmb::TidLease lease_;
  bool haveLease_ = false;
  uint8_t hello_[160];
  uint16_t helloTid_ = 0;
  uint32_t helloCrc_ = 0, helloAt_ = 0;
  // the response in b_.resp: pending (waiting for the sender) or in flight
  bool respPending_ = false, respInFlight_ = false;
  size_t respLen_ = 0;
  // last executed request
  bool haveLastReq_ = false;
  uint32_t lastReqGen_ = 0, lastReqCrc_ = 0, lastReqAt_ = 0;
  uint16_t lastReqTid_ = 0;
  uint32_t statusAt_ = 0;
  uint8_t statusSeq_ = 0;
  Counters c_;
};

}  // namespace panel_core
