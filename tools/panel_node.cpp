// L8 panel mode process for the D10 gateway rig (task 027): lib/panel_core with the REAL
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

static uint8_t rxbuf[zmb::kMaxMessage];
static uint8_t respbuf[zmb::kMaxMessage];
static zmb::TidEntry tidbuf[256];

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
  std::unique_ptr<PanelCore> core;
  bool mute = false, silent = false, pendingSingle = false;
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
    Identity id;
    memcpy(id.mac, mac, 6);
    id.devType = devType;
    memcpy(id.bootId, bootId, 16);
    id.deviceId = deviceId.c_str();
    id.firmware = "l8 panel_node 0.1";
    id.storedPanelId = storedPanelId;
    id.storedAddr = storedAddr;
    Buffers b{rxbuf, sizeof rxbuf, respbuf, sizeof respbuf, tidbuf, sizeof tidbuf / sizeof tidbuf[0]};
    CoreIo io;
    io.ctx = this;
    io.tx = [](void* c, const zmb::CanFrame& f, bool single) { static_cast<Node*>(c)->emit(f, single); };
    io.dispatch = [](void* c, const uint8_t* req, size_t len, uint8_t* resp, size_t cap) {
      return static_cast<Node*>(c)->dispatch(req, len, resp, cap);
    };
    io.statusA = [](void*, uint8_t* flags, uint16_t* rev, uint16_t* saved, uint8_t* chOn, uint8_t* chFault) {
      uint32_t sv = 0;
      bool hs = settingsSavedRevision(sv);
      *flags = uint8_t((drive.masterOn ? 1 : 0) | (settingsPreviewActive() ? 2 : 0) | 4);
      *rev = uint16_t(settingsRevision());
      *saved = hs ? uint16_t(sv) : 0xFFFF;
      *chOn = 0;
      *chFault = 0;
    };
    io.log = [](void* c, const char* t) { static_cast<Node*>(c)->out.push_back(std::string("E ") + t); };
    core.reset(new PanelCore(id, b, io, xorshift, &rng));
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
  h.info = [](JsonObject o) {
    o["product"] = "ZVER L8";
    o["deviceId"] = g->deviceId.c_str();
    o["bootId"] = g->bootHex.c_str();
  };
  h.status = [](JsonObject o) {
    o["configRevision"] = settingsRevision();
    uint32_t s;
    if (settingsSavedRevision(s)) o["savedRevision"] = s;
    else o["savedRevision"] = nullptr;
    o["bootId"] = g->bootHex.c_str();
    o["master"] = drive.masterOn;
    o["previewActive"] = settingsPreviewActive();
  };
  l8proto::setHooks(h);
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
      n.core->tick(n.now);
    } else if (!strcmp(cmd, "F")) {
      zmb::CanFrame f;
      f.id = uint32_t(strtoul(strtok_r(nullptr, " \r\n", &rest), nullptr, 16));
      auto d = hexBytes(strtok_r(nullptr, " \r\n", &rest));
      memcpy(f.data, d.data(), d.size() < 8 ? d.size() : 8);
      // the rig delivers frames as they come: the queue was empty just before
      panel_rx::QueueStamp q;
      panel_rx::Stamp st = q.read(q.poll(panel_rx::Snapshot::at(n.now, true, false)), n.now, false);
      n.core->onFrame(f, st, n.now);
    } else if (!strcmp(cmd, "R")) {
      if (n.pendingSingle) {
        n.core->singleShotResult(atoi(strtok_r(nullptr, " \r\n", &rest)) ? SingleShot::Sent : SingleShot::Lost, n.now);
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
    for (auto& l : n.out) puts(l.c_str());
    puts(".");
    fflush(stdout);
  }
  return 0;
}
