// L8 panel mode process for the D10 gateway rig (task 027; task 036: built on the firmware's
// src/panel_mode/PanelMode with a line-protocol PanelPort): lib/panel_core with the REAL
// command layer l8proto (src/protocol, src/settings) built for the host over the same test
// stubs as test_protocol (test/support). A REQUEST from the bus runs through
// l8proto::handleV1(..., Transport::Usb, true, ...) — the code HTTP and USB use. Not the
// firmware: flash is the fake LittleFS, outputs/power/IMU are hooks, time is the rig's.
//
// Same line protocol as tools/zmb_node.cpp (argv: <mac 12 hex> <seed> [devType]; commands
// T, F, R, REBOOT, MUTE, SILENT; output X, S, E, "."), plus:
//   DIRECT <json>   the same dispatcher without the bus -> "D <response>" (parity reference)
// The journal is the dispatcher's: "E exec <cmd> <id> <revision> <stateHash>" after it returned.
#include <Arduino.h>
#include <LittleFS.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "../src/settings/settings.cpp"
#include "../src/protocol/protocol.cpp"
#include "panel_core.h"
#include "../src/panel_mode/panel_mode.cpp"
#include <deque>

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

using namespace panel_core;

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


struct Node;
static Node* g = nullptr;

static std::string stateHash() {
  uint32_t saved = 0;
  bool hs = settingsSavedRevision(saved);
  std::string s = settingsJson(true).c_str();
  s += "|" + std::to_string(settingsRevision()) + "|" + (hs ? std::to_string(saved) : "-") + "|" +
       std::to_string(settingsPreviewActive()) + settingsPreviewOwner() + "|" + std::to_string(drive.masterOn);
  char b[20];
  snprintf(b, sizeof b, "%016zx", std::hash<std::string>()(s));
  return b;
}

struct Node {
  uint8_t mac[6];
  uint8_t devType;
  uint32_t rng;
  uint8_t bootId[16];
  std::string deviceId, bootHex;
  // the firmware's panel mode over a port that is this process's line protocol
  struct LinePort : panel_mode::PanelPort {
    Node* n = nullptr;
    std::deque<zmb::CanFrame> q;
    bool ssReady = false;
    SingleShot ssResult = SingleShot::Sent;
    bool read(zmb::CanFrame& f) override {
      if (q.empty()) return false;
      f = q.front();
      q.pop_front();
      return true;
    }
    bool send(const zmb::CanFrame& f, bool single) override;
    bool rxQueueEmpty() override { return q.empty(); }
    uint32_t overrunCount() override { return 0; }
    bool busOff() override { return false; }
    void setListenOnly(bool l) override { listenOnly = l; }  // A5: the core sends only when normal
    bool listenOnly = true;
    bool singleShotDone(SingleShot& r) override {
      if (!ssReady) return false;
      ssReady = false;
      r = ssResult;
      return true;
    }
  };
  struct LineStore : panel_mode::PanelStore {
    uint32_t id = 0;
    uint8_t addr = 0;
    bool load(uint32_t& i, uint8_t& a) override {
      i = id;
      a = addr;
      return true;  // as the rig passes them, zero or not
    }
    void save(uint32_t, uint8_t) override {}  // the rig keeps them across REBOOT itself
  };
  LinePort port;
  LineStore store;
  std::unique_ptr<panel_mode::PanelMode> mode;
  PanelCore* core = nullptr;  // mode->core()
  bool mute = false, silent = false, pendingSingle = false;
  int reportedListen = -1;
  uint32_t now = 0;
  std::vector<std::string> out;

  Node(const uint8_t m[6], uint32_t seed, uint8_t type) : devType(type), rng(seed ? seed : 1) {
    memcpy(mac, m, 6);
    boot(0, 0);
  }

  void boot(uint32_t storedPanelId, uint8_t storedAddr) {
    for (auto& b : bootId) b = uint8_t(xorshift(&rng));
    char buf[8];
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
    if (!mode) {
      port.n = this;
      panel_mode::Host h;
      h.ctx = this;
      h.now = [](void* c) { return static_cast<Node*>(c)->now; };
      h.dispatch = [](void* c, const uint8_t* req, size_t len, uint8_t* resp, size_t cap) {
        return static_cast<Node*>(c)->dispatch(req, len, resp, cap);
      };
      h.statusA = [](void*, uint8_t* flags, uint16_t* rev, uint16_t* saved, uint8_t* chOn, uint8_t* chFault) {
        uint32_t sv = 0;
        bool hs = settingsSavedRevision(sv);
        *flags = uint8_t((drive.masterOn ? 1 : 0) | (settingsPreviewActive() ? 2 : 0) | 4);
        *rev = uint16_t(settingsRevision());
        *saved = hs ? uint16_t(sv) : 0xFFFF;
        *chOn = 0;
        *chFault = 0;
      };
      h.log = [](void* c, const char* t) { static_cast<Node*>(c)->out.push_back(std::string("E ") + t); };
      h.rand = xorshift;
      h.randCtx = &rng;
      mode.reset(new panel_mode::PanelMode(port, store, h));
    }
    store.id = storedPanelId;
    store.addr = storedAddr;
    port.q.clear();
    mode->enter(mac, devType, bootId, deviceId.c_str(), "l8 panel_node 0.1", 3);
    core = mode->core();
  }

  void emit(const zmb::CanFrame& f, bool single) {
    if (silent) return;
    char line[64];
    snprintf(line, sizeof line, "%s %08X %02X%02X%02X%02X%02X%02X%02X%02X", single ? "S" : "X", unsigned(f.id), f.data[0],
             f.data[1], f.data[2], f.data[3], f.data[4], f.data[5], f.data[6], f.data[7]);
    out.push_back(line);
    if (single) pendingSingle = true;
  }

  std::string run(const char* data, size_t len) {
    testMillis = now;
    int http = 0;
    String r = l8proto::handleV1(data, len, l8proto::Transport::Usb, true, http);
    StaticJsonDocument<256> filter;
    filter["cmd"] = true;
    filter["id"] = true;
    DynamicJsonDocument d(512);
    std::string cmd = "-", id = "-";
    if (!deserializeJson(d, data, len, DeserializationOption::Filter(filter)) && d.is<JsonObject>()) {
      if (d["cmd"].is<const char*>()) cmd = d["cmd"].as<const char*>();
      if (d["id"].is<const char*>()) id = d["id"].as<const char*>();
    }
    out.push_back("E exec " + cmd + " " + id + " " + std::to_string(settingsRevision()) + " " + stateHash() + " t=" +
                  std::to_string(now));
    return r.c_str();
  }

  size_t dispatch(const uint8_t* req, size_t len, uint8_t* resp, size_t cap) {
    out.push_back("E req " + std::string(reinterpret_cast<const char*>(req), len));
    std::string r = run(reinterpret_cast<const char*>(req), len);
    if (mute || r.size() > cap) return 0;
    memcpy(resp, r.data(), r.size());
    return r.size();
  }
};

static void installHooks() {
  l8proto::Hooks h;
  h.bootId = [] { return g->bootHex.c_str(); };
  h.otaBusy = [] { return false; };
  h.powerReady = [] { return true; };
  h.imuOk = [] { return true; };
  // The same fields as the firmware's infoFill/statusFill (src/hub/hub.cpp): identity,
  // protocol, capabilities and limits; what the host does not have is neutral (no OTA, no
  // IMU, outputs off). Task 029: the app's client checks this descriptor by contract.
  h.info = [](JsonObject d) {
    d["product"] = "ZVER L8";
    d["hardware"] = 3;
    d["deviceId"] = g->deviceId.c_str();
    d["bootId"] = g->bootHex.c_str();
    d["firmware"] = "l8 panel_node 0.1";
    d["build"] = "host";
    auto protocol = d.createNestedObject("protocol");
    protocol["major"] = 1;
    protocol["minor"] = 0;
    d["configSchema"] = L8_CONFIG_SCHEMA;
    auto transports = d.createNestedArray("transports");
    transports.add("http");
    transports.add("usb");
    auto caps = d.createNestedArray("capabilities");
    for (const char* c : {"info.v1", "channel.reason", "config.savedRevision", "commands.v1", "preview.v1", "status.sse"}) caps.add(c);
    auto l = d.createNestedObject("limits");
    l["channels"] = 8;
    l["maxCommandBytes"] = l8proto::MAX_COMMAND_BYTES;
    l["maxConfigBytes"] = L8_MAX_CONFIG_BYTES;
    l["maxLedCount"] = 1000;
    l["maxChannelAmps"] = L8_MAX_CHANNEL_AMPS;
    l["previewLeaseMs"] = L8_PREVIEW_LEASE_MS;
    l["previewIntervalMs"] = 100;
    l["statusIntervalMs"] = 500;
    l["maxSseClients"] = 3;  // hub.cpp: streams[3]
  };
  h.status = [](JsonObject d) {
    d["configRevision"] = settingsRevision();
    uint32_t s;
    if (settingsSavedRevision(s)) d["savedRevision"] = s;
    else d["savedRevision"] = nullptr;
    d["bootId"] = g->bootHex.c_str();
    d["previewActive"] = settingsPreviewActive();
    if (settingsPreviewActive()) d["previewOwner"] = settingsPreviewOwner();
    else d["previewOwner"] = nullptr;
    d["previewRemainingMs"] = settingsPreviewRemainingMs();
    d["build"] = "host";
    d["product"] = "ZVER L8";
    d["hardware"] = 3;
    d["firmware"] = "l8 panel_node 0.1";
    d["uptimeMs"] = millis();
    d["master"] = drive.masterOn;
    d["vbat"] = 0;
    d["ign"] = false;
    d["ioOk"] = true;
    d["ioFault"] = false;
    d["totalAmps"] = 0;
    d["network"] = "host";
    d["ip"] = "";
    auto channels = d.createNestedArray("channels");
    for (int i = 0; i < 8; ++i) {
      auto c = channels.createNestedObject();
      c["on"] = false;
      c["amps"] = 0;
      c["fault"] = "";
      c["reason"] = "host";
    }
  };
  l8proto::setHooks(h);
}

bool Node::LinePort::send(const zmb::CanFrame& f, bool single) {
  n->emit(f, single);
  return true;  // on the rig's bus (or dropped by SILENT, which the core never learns)
}

int main(int argc, char** argv) {
  if (argc < 3) return 2;
  auto m = hexBytes(argv[1]);
  if (m.size() != 6) return 2;
  uint32_t seed = uint32_t(strtoul(argv[2], nullptr, 10));
  uint8_t type = argc > 3 ? uint8_t(atoi(argv[3])) : 1;
  settingsBegin();
  settingsSave();
  settingsSaveNow();
  Node n(m.data(), seed, type);
  g = &n;
  installHooks();
  static char line[1 << 16];
  setvbuf(stdout, nullptr, _IOFBF, 1 << 16);
  while (fgets(line, sizeof line, stdin)) {
    char* rest = nullptr;
    char* cmd = strtok_r(line, " \r\n", &rest);
    if (!cmd) continue;
    n.out.clear();
    if (!strcmp(cmd, "T")) {
      n.now = uint32_t(strtoul(strtok_r(nullptr, " \r\n", &rest), nullptr, 10));
      testMillis = n.now;
      settingsTick();  // as main.cpp's loop: preview lease expiry, delayed flash write
      n.mode->tick();
    } else if (!strcmp(cmd, "F")) {
      zmb::CanFrame f;
      f.id = uint32_t(strtoul(strtok_r(nullptr, " \r\n", &rest), nullptr, 16));
      auto d = hexBytes(strtok_r(nullptr, " \r\n", &rest));
      memcpy(f.data, d.data(), d.size() < 8 ? d.size() : 8);
      // the rig delivers frames as they come: the queue was empty just before
      n.mode->rx();  // the queue is empty just before the frame: the anchor is now (§14.3)
      n.port.q.push_back(f);
      n.mode->rx();
    } else if (!strcmp(cmd, "R")) {
      if (n.pendingSingle) {
        n.port.ssResult = atoi(strtok_r(nullptr, " \r\n", &rest)) ? SingleShot::Sent : SingleShot::Lost;
        n.port.ssReady = true;
        n.mode->service();
        n.pendingSingle = false;
      }
    } else if (!strcmp(cmd, "REBOOT")) {
      uint8_t keep = n.core->addr() ? n.core->addr() : n.core->discovery().storedAddr();
      uint32_t pid = n.core->discovery().panelId();
      settingsBegin();  // RAM state (preview, editor) is gone; flash stays
      n.boot(pid, keep);
      n.pendingSingle = false;
      n.out.push_back("E rebooted " + n.bootHex);
    } else if (!strcmp(cmd, "MUTE")) {
      n.mute = atoi(strtok_r(nullptr, " \r\n", &rest)) != 0;
    } else if (!strcmp(cmd, "SILENT")) {
      n.silent = atoi(strtok_r(nullptr, " \r\n", &rest)) != 0;
    } else if (!strcmp(cmd, "DIRECT")) {
      std::string json = rest ? rest : "";
      while (!json.empty() && (json.back() == '\n' || json.back() == '\r')) json.pop_back();
      std::string r = n.run(json.data(), json.size());
      n.out.push_back("D " + r);
    } else if (!strcmp(cmd, "STATE")) {
      n.out.push_back("E state " + std::to_string(settingsRevision()) + " " + stateHash());
    }
    // the rig's bus model needs to know who acknowledges frames (A5 rev 2): M 1 listen, M 0 normal
    int listening = n.mode->listening() ? 1 : 0;
    if (listening != n.reportedListen) {
      n.reportedListen = listening;
      n.out.push_back("M " + std::to_string(listening));
    }
    for (auto& l : n.out) puts(l.c_str());
    puts(".");
    fflush(stdout);
  }
  return 0;
}
