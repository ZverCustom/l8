// ZMB module process for integration tests of the D10 gateway (task 016). Built from the
// real lib/zmb (ModuleDiscovery, Sender, Receiver, TidTable) with ASan/UBSan. It is a test
// participant, not the L8 firmware: its business answers are scripted (info, status,
// preview, save, get_config, apply) and prove the route, not the firmware dispatcher, flash
// writes or power outputs.
//
// argv: <mac 12 hex> <seed> [devType]
// stdin, one command per line; every command is answered by output lines and a final ".":
//   T <ms>          time passes: timers, periodic STATUS_A/BEACON, discovery
//   F <id> <hex>    a frame from the bus (all frames, the node filters)
//   R <1|0>         result of the single-shot frame printed as "S" just before: sent / lost
//   REBOOT          new boot: new bootId, discovery and transport start over
//   MUTE <0|1>      1: requests are executed but no RESPONSE is sent
//   SILENT <0|1>    1: the node transmits nothing at all
// stdout:
//   X <id> <hex>    transport/periodic frame to put on the bus now
//   S <id> <hex>    single-shot frame (ANNOUNCE/BEACON): the bus answers with R
//   E <what> ...    log: exec <cmd> <id> <revision>, hello_ack <status>, ready <addr>, ...
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <memory>
#include <string>
#include <vector>

#include "zmb.h"
#include "zmb_disc.h"

using namespace zmb;

static std::vector<uint8_t> hexBytes(const char* s) {
  std::vector<uint8_t> v;
  for (size_t i = 0; s && s[i] && s[i + 1]; i += 2) {
    char b[3] = {s[i], s[i + 1], 0};
    v.push_back(uint8_t(strtoul(b, nullptr, 16)));
  }
  return v;
}

static uint32_t xorshift(void* ctx) {
  uint32_t& x = *static_cast<uint32_t*>(ctx);
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  return x;
}

static std::string field(const std::string& json, const char* key) {
  std::string k = std::string("\"") + key + "\":\"";
  size_t at = json.find(k);
  if (at == std::string::npos) return "";
  at += k.size();
  size_t end = json.find('"', at);
  return end == std::string::npos ? "" : json.substr(at, end - at);
}

static uint8_t rxbuf[kMaxMessage];
static TidEntry tidbuf[256];

struct Node {
  uint8_t mac[6];
  uint8_t devType = 1;
  uint32_t rng;
  ModuleDiscovery disc;
  uint8_t bootId[16];
  uint32_t bootTag = 0;
  std::string deviceId, bootHex;
  std::unique_ptr<Sender> tx;
  std::unique_ptr<Receiver> rx;
  TidTable tids{tidbuf, sizeof tidbuf / sizeof tidbuf[0], 1};
  uint32_t lastGen = 0, tidGen = 0;
  bool haveTidGen = false;
  enum class Phase { Unassigned, Hello, Ready } phase = Phase::Unassigned;
  std::vector<uint8_t> txmsg;  // message the sender reads in place
  TidLease lease;
  bool haveLease = false;
  uint16_t helloTid = 0;
  uint32_t helloCrc = 0, helloAt = 0;
  std::vector<std::vector<uint8_t>> queue;  // responses waiting for the sender (≤ 1)
  int revision = 1, saved = 1;
  uint32_t statusAt = 0;
  uint8_t statusSeq = 0;
  bool mute = false, silent = false;
  bool pendingSingle = false;
  std::vector<std::string> out;

  Node(const uint8_t m[6], uint32_t seed, uint8_t type) : devType(type), rng(seed ? seed : 1), disc(m, type, xorshift, &rng) {
    memcpy(mac, m, 6);
    newBoot();
  }

  void newBoot() {
    for (auto& b : bootId) b = uint8_t(xorshift(&rng));
    bootTag = (uint32_t(bootId[0]) << 24) | (uint32_t(bootId[1]) << 16) | (uint32_t(bootId[2]) << 8) | bootId[3];
    char buf[40];
    bootHex.clear();
    for (auto b : bootId) {
      snprintf(buf, sizeof buf, "%02x", b);
      bootHex += buf;
    }
    deviceId = devType == 1 ? "L8-" : "K8-";
    for (auto b : mac) {
      snprintf(buf, sizeof buf, "%02X", b);
      deviceId += buf;
    }
    disc.setBootLo(uint8_t(bootTag));
  }

  void emit(const char* tag, const CanFrame& f) {
    if (silent) return;
    char line[64];
    snprintf(line, sizeof line, "%s %08X %02X%02X%02X%02X%02X%02X%02X%02X", tag, unsigned(f.id), f.data[0], f.data[1], f.data[2],
             f.data[3], f.data[4], f.data[5], f.data[6], f.data[7]);
    out.push_back(line);
  }
  void log(const std::string& s) { out.push_back("E " + s); }

  RxContext ctx() const {
    RxContext c;
    c.local = disc.addr();
    c.side = Side::Module;
    c.panelGen = disc.panelGen();
    c.ownBootTag = bootTag;
    return c;
  }

  void finishLease(uint32_t now) {
    if (haveLease) tids.finish(lease, now);
    haveLease = false;
  }

  // Starts `msg` (header filled here except type-specific fields) with a fresh tid.
  bool start(MsgType t, const std::vector<uint8_t>& payload, uint16_t reqTid, uint32_t reqCrc, uint32_t now) {
    if (!tx || tx->state() != TxState::Idle) return false;
    TidLease l;
    if (!tids.alloc(now, l)) {
      log("tidwait");
      return false;
    }
    Header h;
    h.mtype = uint8_t(t);
    h.tid = l.tid;
    h.reqTid = reqTid;
    h.panelGen = disc.panelGen();
    h.bootTag = bootTag;
    h.reqCrc = reqCrc;
    txmsg.assign(kHeaderLen, 0);
    h.encode(txmsg.data());
    txmsg.insert(txmsg.end(), payload.begin(), payload.end());
    if (!tx->start(txmsg.data(), txmsg.size(), l.tid)) {
      tids.finish(l, now);
      return false;
    }
    lease = l;
    haveLease = true;
    if (t == MsgType::Hello) {
      helloTid = l.tid;
      helloCrc = crc32(txmsg.data(), txmsg.size());
      helloAt = now;
    }
    return true;
  }

  void sendHello(uint32_t now) {
    Hello hl;
    hl.devType = devType;
    hl.deviceId = deviceId.c_str();
    hl.deviceIdLen = uint8_t(deviceId.size());
    memcpy(hl.bootId, bootId, 16);
    static const char fw[] = "zmb-node 0.1";
    hl.firmware = fw;
    hl.firmwareLen = uint8_t(strlen(fw));
    hl.caps = 0x1F;
    hl.maxMessage = 24576;
    hl.hwRev = 3;
    uint8_t buf[128];
    size_t n = hl.encode(buf, sizeof buf);
    if (start(MsgType::Hello, std::vector<uint8_t>(buf, buf + n), 0, 0, now)) phase = Phase::Hello;
  }

  std::string execute(const std::string& json) {
    std::string id = field(json, "id"), cmd = field(json, "cmd");
    std::string head = "{\"v\":1,\"id\":\"" + id + "\",";
    std::string r;
    if (cmd == "info") {
      r = head + "\"ok\":true,\"result\":{\"deviceId\":\"" + deviceId + "\",\"bootId\":\"" + bootHex + "\",\"model\":\"L8\"}}";
    } else if (cmd == "status") {
      r = head + "\"ok\":true,\"result\":{\"deviceId\":\"" + deviceId + "\",\"bootId\":\"" + bootHex +
          "\",\"revision\":" + std::to_string(revision) + ",\"savedRevision\":" + std::to_string(saved) + "}}";
    } else if (cmd == "preview") {
      revision++;
      r = head + "\"ok\":true,\"result\":{\"revision\":" + std::to_string(revision) + "}}";
    } else if (cmd == "save") {
      saved = revision;
      r = head + "\"ok\":true,\"revision\":" + std::to_string(revision) + ",\"savedRevision\":" + std::to_string(saved) +
          ",\"result\":{\"model\":\"scripted\"}}";
    } else if (cmd == "get_config") {
      r = head + "\"ok\":true,\"result\":{\"pad\":\"" + std::string(20000, 'c') + "\"}}";
    } else if (cmd == "apply") {
      revision++;
      r = head + "\"ok\":true,\"result\":{\"bytes\":" + std::to_string(json.size()) + ",\"revision\":" + std::to_string(revision) + "}}";
    } else {
      r = head + "\"ok\":false,\"error\":{\"code\":\"UNKNOWN_CMD\"}}";
    }
    log("exec " + (cmd.empty() ? std::string("-") : cmd) + " " + (id.empty() ? std::string("-") : id) + " " + std::to_string(revision));
    return r;
  }

  void onMessage(const RxEvent& e, uint32_t now) {
    const uint8_t* m = rx->message();
    if (e.header.mtype == uint8_t(MsgType::HelloAck)) {
      HelloAck a;
      if (phase == Phase::Hello && matchReply(e.header, helloTid, helloCrc, disc.panelGen(), bootTag) &&
          HelloAck::decode(m + kHeaderLen, e.len - kHeaderLen, a) && a.addr == disc.addr()) {
        phase = Phase::Ready;
        statusAt = now;
        log("hello_ack " + std::to_string(a.status));
        log("ready " + std::to_string(disc.addr()));
      }
      return;
    }
    if (e.header.mtype != uint8_t(MsgType::Request) || phase != Phase::Ready) return;
    std::string json(reinterpret_cast<const char*>(m + kHeaderLen), e.len - kHeaderLen);
    std::string resp = execute(json);
    if (mute) return;
    std::vector<uint8_t> payload(resp.begin(), resp.end());
    uint32_t crc = crc32(m, e.len);
    // the response header carries reqTid/reqCrc: keep them with the queued payload
    std::vector<uint8_t> item(6);
    item[0] = uint8_t(e.header.tid >> 8);
    item[1] = uint8_t(e.header.tid);
    item[2] = uint8_t(crc >> 24);
    item[3] = uint8_t(crc >> 16);
    item[4] = uint8_t(crc >> 8);
    item[5] = uint8_t(crc);
    item.insert(item.end(), payload.begin(), payload.end());
    if (queue.empty()) queue.push_back(item);
    else log("drop busy");
  }

  void pump(uint32_t now) {
    if (disc.generation() != lastGen) {  // address lost or changed: transport state ends
      lastGen = disc.generation();
      finishLease(now);
      tx.reset();
      rx.reset();
      queue.clear();
      phase = Phase::Unassigned;
    }
    if (!haveTidGen || tidGen != disc.panelGen()) {  // a new panelGen: a new tid space
      tids.reset(1);
      tidGen = disc.panelGen();
      haveTidGen = true;
      haveLease = false;
    }
    if (disc.assigned() && !tx) {
      tx.reset(new Sender(disc.addr(), 0));
      rx.reset(new Receiver(0, rxbuf, sizeof rxbuf));
      sendHello(now);
    }
    if (phase == Phase::Hello && now - helloAt > 2000) {  // no HELLO_ACK: start over
      log("hello_timeout");
      disc.restart(now);
      pump(now);
      return;
    }
    if (tx && tx->state() == TxState::Idle && !queue.empty()) {
      auto& it = queue.front();
      uint16_t reqTid = uint16_t((it[0] << 8) | it[1]);
      uint32_t crc = (uint32_t(it[2]) << 24) | (uint32_t(it[3]) << 16) | (uint32_t(it[4]) << 8) | it[5];
      std::vector<uint8_t> payload(it.begin() + 6, it.end());
      if (start(MsgType::Response, payload, reqTid, crc, now)) queue.erase(queue.begin());
    }
    CanFrame f;
    while (tx && !silent && tx->poll(now, f)) {
      emit("X", f);
      tx->confirm(now);
      if (haveLease) tids.touch(lease, now);
    }
  }

  void outcome(const TxOutcome& o, uint32_t now) {
    if (o.result == TxResult::None) return;
    finishLease(now);
    log(std::string("tx ") + (o.result == TxResult::Delivered ? "delivered" : o.result == TxResult::Unknown ? "unknown" : "failed"));
  }

  void time(uint32_t now) {
    if (tx) outcome(tx->tick(now), now);
    if (rx) {
      RxEvent e = rx->tick(now);
      if (e.kind != RxKind::None) log("rx_end");
    }
    pump(now);
    if (phase == Phase::Ready && !silent && expired(statusAt, now)) {
      statusAt = now + 500;
      CanFrame s;
      AddrId a{5, sfunc::kStatusA, disc.addr(), 0x3F, 0};
      s.id = a.encode();
      s.data[0] = uint8_t((revision != saved ? 2 : 0) | 4);
      s.data[1] = statusSeq++;
      s.data[2] = uint8_t(revision >> 8);
      s.data[3] = uint8_t(revision);
      s.data[4] = uint8_t(saved >> 8);
      s.data[5] = uint8_t(saved);
      emit("X", s);
    }
    CanFrame single;
    if (!silent && disc.poll(now, single)) {
      emit("S", single);
      pendingSingle = true;
    }
  }

  void frame(const CanFrame& f, uint32_t now) {
    if (disc.onFrame(f, now)) {
      pump(now);
      return;
    }
    if (tx) outcome(tx->onFrame(f, now), now);
    if (rx && disc.assigned()) {
      RxOutput r = rx->onFrame(f, ctx(), now);
      if (r.hasReply) {
        emit("X", r.reply);
        if (r.flow) rx->confirmFlow(r.flow, now);
      }
      if (r.event.kind == RxKind::Message) onMessage(r.event, now);
    }
    pump(now);
  }
};

int main(int argc, char** argv) {
  if (argc < 3) return 2;
  auto m = hexBytes(argv[1]);
  if (m.size() != 6) return 2;
  uint32_t seed = uint32_t(strtoul(argv[2], nullptr, 10));
  uint8_t type = argc > 3 ? uint8_t(atoi(argv[3])) : 1;
  Node n(m.data(), seed, type);
  uint32_t now = 0;
  static char line[256];
  setvbuf(stdout, nullptr, _IOFBF, 1 << 16);
  while (fgets(line, sizeof line, stdin)) {
    char* cmd = strtok(line, " \r\n");
    if (!cmd) continue;
    n.out.clear();
    if (!strcmp(cmd, "T")) {
      now = uint32_t(strtoul(strtok(nullptr, " \r\n"), nullptr, 10));
      n.time(now);
    } else if (!strcmp(cmd, "F")) {
      CanFrame f;
      f.id = uint32_t(strtoul(strtok(nullptr, " \r\n"), nullptr, 16));
      auto d = hexBytes(strtok(nullptr, " \r\n"));
      memcpy(f.data, d.data(), d.size() < 8 ? d.size() : 8);
      n.frame(f, now);
    } else if (!strcmp(cmd, "R")) {
      if (n.pendingSingle) {
        if (atoi(strtok(nullptr, " \r\n"))) n.disc.sent(now);
        else n.disc.lost(now);
        n.pendingSingle = false;
      }
    } else if (!strcmp(cmd, "REBOOT")) {
      uint8_t keep = n.disc.addr() ? n.disc.addr() : n.disc.storedAddr();
      uint32_t pid = n.disc.panelId();
      n.disc = ModuleDiscovery(n.mac, n.devType, xorshift, &n.rng);
      n.disc.setStored(pid, keep);
      n.newBoot();
      n.lastGen = n.disc.generation();
      n.tx.reset();
      n.rx.reset();
      n.queue.clear();
      n.haveLease = false;
      n.haveTidGen = false;
      n.phase = Node::Phase::Unassigned;
      n.revision = n.saved;
      n.log("rebooted " + n.bootHex);
    } else if (!strcmp(cmd, "MUTE")) {
      n.mute = atoi(strtok(nullptr, " \r\n")) != 0;
    } else if (!strcmp(cmd, "SILENT")) {
      n.silent = atoi(strtok(nullptr, " \r\n")) != 0;
    }
    for (auto& l : n.out) puts(l.c_str());
    puts(".");
    fflush(stdout);
  }
  return 0;
}
