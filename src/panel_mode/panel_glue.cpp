#include "panel_glue.h"

#include <Preferences.h>
#include <esp_system.h>

#include "can_bus/panel_port_twai.h"
#include "l8_version.h"
#include "panel_mode.h"
#include "power/power.h"
#include "protocol/protocol.h"
#include "settings/settings.h"
#include "system/system.h"
#include "vehicle/vehicle.h"

extern CANBus canBus;  // main.cpp

namespace {

class NvsStore : public panel_mode::PanelStore {
 public:
  bool load(uint32_t& id, uint8_t& addr) override {
    Preferences p;
    if (!p.begin("panel", true)) return false;
    id = p.getULong("id", 0);
    addr = p.getUChar("addr", 0);
    p.end();
    return id != 0 && addr != 0;
  }
  void save(uint32_t id, uint8_t addr) override {
    Preferences p;
    if (!p.begin("panel", false)) return;
    p.putULong("id", id);
    p.putUChar("addr", addr);
    p.end();
  }
};

TwaiPanelPort port;
NvsStore store;
panel_mode::PanelMode* mode = nullptr;  // allocated on enter: no RAM cost while the mode is off
char deviceId[20];
uint8_t mac[6];
uint8_t bootId[16];

uint32_t nowMs(void*) { return millis(); }
uint32_t rnd(void*) { return esp_random(); }

size_t dispatch(void*, const uint8_t* req, size_t len, uint8_t* resp, size_t cap) {
  int http = 0;
  String r = l8proto::handleV1(reinterpret_cast<const char*>(req), len, l8proto::Transport::Usb, true, http);
  if (r.length() > cap) return 0;
  memcpy(resp, r.c_str(), r.length());
  return r.length();
}

void statusA(void*, uint8_t* flags, uint16_t* rev, uint16_t* saved, uint8_t* chOn, uint8_t* chFault) {
  PowerState ps = powerSnapshot();
  uint8_t on = 0, fault = 0;
  for (int i = 0; i < 8; i++) {
    if (ps.channels[i].on) on |= uint8_t(1u << i);
    if (ps.channels[i].fault) fault |= uint8_t(1u << i);
  }
  bool rpmFresh = mode && (mode->view(millis()).fresh & panel_core::kFreshRpm);
  *flags = uint8_t((drive.masterOn ? 1 : 0) | (settingsPreviewActive() ? 2 : 0) | (ps.ioOk ? 4 : 0) |
                   (fault ? 8 : 0) | (rpmFresh ? 16 : 0));
  uint32_t sv = 0;
  bool hs = settingsSavedRevision(sv);
  *rev = uint16_t(settingsRevision());
  *saved = hs ? uint16_t(sv) : 0xFFFF;
  *chOn = on;
  *chFault = fault;
}

void logLine(void*, const char* t) { sysLog(String("panel: ") + t); }

}  // namespace

void panelModeEnter() {
  port.bus = &canBus;
  if (!mode) {
    panel_mode::Host h;
    h.now = nowMs;
    h.dispatch = dispatch;
    h.statusA = statusA;
    h.log = logLine;
    h.rand = rnd;
    mode = new panel_mode::PanelMode(port, store, h);
  }
  esp_efuse_mac_get_default(mac);
  esp_fill_random(bootId, sizeof bootId);
  snprintf(deviceId, sizeof deviceId, "L8-%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  mode->enter(mac, 1, bootId, deviceId, L8_FW_VERSION, L8_HW_REV);
  sysLog(String("panel mode: ") + deviceId);
}

void panelModeExit() {
  if (!mode) return;
  mode->exit();
  delete mode;
  mode = nullptr;
  vehicle = VehicleData();
  vehicleFresh = 0;
}

bool panelModeActive() { return mode && mode->active(); }

void panelModeTick() {
  if (!panelModeActive()) return;
  mode->tick();
  panel_core::VehicleView v = mode->view(millis());
  vehicle.rpm = v.rpm;
  vehicle.speed = v.speed;
  vehicle.throttle = v.throttle;
  vehicle.temp = v.temp;
  vehicle.lastUpdate = v.lastUpdate;
  vehicleFresh = v.fresh;
}

String panelModeStatus() {
  if (!panelModeActive()) return F("panel off");
  if (mode->listening()) {
    // A5: what the module waits for
    uint32_t retry = mode->retryInMs(millis());
    switch (mode->listenReason()) {
      case panel_mode::Listen::BusOff:
      case panel_mode::Listen::Limit:
        if (retry) {
          return String("panel listen: ") + (mode->listenReason() == panel_mode::Listen::Limit ? String("bus-off limit") : String("bus-off ") + mode->busOffRun()) +
                 ", retry in " + String((retry + 999) / 1000) + " s";
        }
        return String("panel listen: bus-off ") + mode->busOffRun() + ", waiting PANEL_HB " + mode->hbCount() + "/3";
      case panel_mode::Listen::NoHb:
        return String("panel listen: no PANEL_HB, waiting ") + mode->hbCount() + "/3";
      default:
        return String("panel listen: waiting PANEL_HB ") + mode->hbCount() + "/3";
    }
  }
  String s = String("panel active ") + mode->phase();
  if (mode->core()->addr()) {
    char b[8];
    snprintf(b, sizeof b, " 0x%02X", mode->core()->addr());
    s += b;
  }
  uint8_t f = vehicleFresh;
  s += !vehicle.lastUpdate ? " veh none" : (f & panel_core::kFreshRpm) ? " veh fresh" : " veh stale";
  s += " busoff=" + String(mode->counters().busOffs);
  return s;
}
