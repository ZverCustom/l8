#include "l8_version.h"
#include "hub.h"
#include <WebServer.h>
#include <lwip/sockets.h>
#include <errno.h>
#include <WiFi.h>
#include <Update.h>
#include <LittleFS.h>
#include <esp_ota_ops.h>
#include <esp32s3/spiram.h>
#include <esp_mac.h>
#include "settings/settings.h"
#include "net/net.h"
#include "power/power.h"
#include "imu/imu.h"
#include "vehicle/vehicle.h"
#include "system/system.h"
#include "board/board.h"
#include <channel_map.h>
#include <v3_safety.h>
#include "l8_ui.h"
#include "led_control/led_control.h"
#include "protocol/protocol.h"
#include <line_framer.h>
class StreamWebServer: public WebServer {
public:
    using WebServer::WebServer;
    WiFiClient detach() { auto client=_currentClient; _currentClient=WiFiClient(); _currentStatus=HC_NONE; _chunked=false; return client; }
};
static StreamWebServer server(80);
struct StatusStream { WiFiClient client; String pending; size_t offset=0; uint32_t queuedAt=0; bool v1=false; };
static uint32_t sseSeq=0;
static StatusStream streams[3];
static void streamTick() {
    static uint32_t lastStatus=0;
    uint32_t now=millis(); bool due=now-lastStatus>=500; String payload, payloadV1;
    for(auto& s:streams) {
        if(!s.client) continue;
        if(!s.client.connected() || (s.pending.length() && now-s.queuedAt>2000)) { s.client.stop(); s.pending=""; continue; }
        if(due && !s.pending.length()) {
            // One snapshot per tick for each format; v1 carries seq/bootId/revisions around it.
            if(s.v1) { if(!payloadV1.length()) payloadV1="event: status\ndata: "+l8proto::sseV1(++sseSeq)+"\n\n"; s.pending=payloadV1; }
            else { if(!payload.length()) payload="event: status\ndata: "+hubStatusJson()+"\n\n"; s.pending=payload; }
            s.offset=0; s.queuedAt=now;
        }
        if(!s.pending.length()) continue;
        // Bounded non-blocking writes: a slow phone cannot stall safety/config.
        int n=::send(s.client.fd(),s.pending.c_str()+s.offset,min(size_t(1024),s.pending.length()-s.offset),MSG_DONTWAIT);
        if(n>0) { s.offset+=n; if(s.offset==s.pending.length()) s.pending=""; }
        else if(n<0 && errno!=EWOULDBLOCK && errno!=EAGAIN) { s.client.stop(); s.pending=""; }
    }
    if(due) lastStatus=now;
}
static String token, deviceId, bootId;
static LineFramer usbFramer(l8proto::MAX_COMMAND_BYTES);
static uint32_t rebootAt=0, reconnectAt=0;
static bool updating=false, uploadOk=false;
static String uploadError;
static bool bootConfirmed=false;
static void syncHubInhibit() {
    powerInhibit(!ledsHealthy() || v3::hubInhibited(bootConfirmed,updating,uploadOk,rebootAt!=0));
}
// Single source for GET /api/info, USB/v1 info. Capabilities list only what this build implements.
static void infoFill(JsonObject d) {
    d["product"]="ZVER L8"; d["hardware"]=3; d["deviceId"]=deviceId; d["bootId"]=bootId;
    d["firmware"]=L8_FW_VERSION; d["build"]=GIT_REV;
    auto protocol=d.createNestedObject("protocol"); protocol["major"]=1; protocol["minor"]=0;
    d["configSchema"]=L8_CONFIG_SCHEMA;
    auto transports=d.createNestedArray("transports"); transports.add("http"); transports.add("usb");
    auto caps=d.createNestedArray("capabilities"); caps.add("info.v1"); caps.add("channel.reason"); caps.add("config.savedRevision"); caps.add("commands.v1"); caps.add("preview.v1"); caps.add("status.sse"); caps.add("ota.http");
    auto l=d.createNestedObject("limits");
    l["channels"]=8; l["maxCommandBytes"]=l8proto::MAX_COMMAND_BYTES; l["maxConfigBytes"]=L8_MAX_CONFIG_BYTES;
    l["maxLedCount"]=MAX_LEDS; l["maxChannelAmps"]=L8_MAX_CHANNEL_AMPS; l["previewLeaseMs"]=L8_PREVIEW_LEASE_MS;
    l["previewIntervalMs"]=100; l["statusIntervalMs"]=500; l["maxSseClients"]=sizeof(streams)/sizeof(streams[0]);
}
String hubInfoJson() { return l8proto::infoJson(); }
static void addSaved(JsonObject d) { uint32_t saved; if(settingsSavedRevision(saved)) d["savedRevision"]=saved; else d["savedRevision"]=nullptr; }
static void statusFill(JsonObject d) {
    auto p=powerSnapshot();
    d["configRevision"]=settingsRevision(); addSaved(d); d["bootId"]=bootId; d["previewActive"]=settingsPreviewActive();
    if(settingsPreviewActive()) d["previewOwner"]=settingsPreviewOwner(); else d["previewOwner"]=nullptr; d["previewRemainingMs"]=settingsPreviewRemainingMs(); d["build"]=GIT_REV; d["ledBackend"]=ledsBackend(); d["ledHealthy"]=ledsHealthy(); d["product"]="ZVER L8"; d["hardware"]=3; d["firmware"]=L8_FW_VERSION; d["uptimeMs"]=millis();
    d["master"]=drive.masterOn; d["vbat"]=p.vbat; d["ign"]=p.ign; d["hold"]=p.hold;
    d["allowWithoutIgn"]=savedData.allowWithoutIgn; d["ledPowerAllowed"]=p.ledPowerAllowed;
    d["ioOk"]=p.ioOk; d["ioFault"]=p.ioLatched; d["powerStatus"]=powerStatusString();
    d["totalBrightnessScale"]=p.totalBrightnessScale; d["totalAmps"]=p.ledAmps; d["sensorOn"]=p.sensorOn; d["sensorFault"]=p.sensorFault;
    d["heap"]=ESP.getFreeHeap(); d["psram"]=psramFound()?esp_spiram_get_size():0; d["flash"]=ESP.getFlashChipSize();
    d["network"]=netStatusString(); d["ap"]=netApOn(); d["apSsid"]=AP_SSID;
    auto wifi=d.createNestedObject("wifi"); wifi["phase"]=netPhase(); wifi["attempts"]=netAttempts();
    wifi["lastDisconnectReason"]=netLastDisconnectReason(); wifi["apClients"]=netApOn()?WiFi.softAPgetStationNum():0;
    wifi["droppedEvents"]=netDroppedEvents();
    d["ip"]=WiFi.status()==WL_CONNECTED?WiFi.localIP().toString():WiFi.softAPIP().toString();
    d["canStatus"]=canStatusString(); d["canBitrate"]=canBitrateString(); d["dataFresh"]=vehicleDataFresh();
    d["rpm"]=vehicle.rpm; d["speed"]=vehicle.speed; d["temp"]=vehicle.temp; d["throttle"]=vehicle.throttle;
    auto channels=d.createNestedArray("channels");
    for(int i=0;i<8;++i) { auto c=channels.createNestedObject(); c["brightnessScale"]=p.channels[i].brightnessScale; c["retries"]=p.channels[i].retries; c["retryPending"]=p.channels[i].retryPending; c["retryInMs"]=p.channels[i].retryInMs; c["on"]=p.channels[i].on; c["amps"]=p.channels[i].amps; c["fault"]=powerFaultName(p.channels[i].fault); c["reason"]=powerReasonName(p.channels[i].reason); c["isVolts"]=p.channels[i].isVolts; c["data"]=bool(boardDataMask()&v3::channelMask(i)); c["legacyIndex"]=v3::hardwareChannel(i); auto pins=c.createNestedArray("pins"); pins.add(v3::CHANNELS[i].power); pins.add(v3::CHANNELS[i].data); pins.add(v3::CHANNELS[i].ground); }
    auto inputs=d.createNestedArray("inputs"); for(int i=0;i<3;++i) { auto x=inputs.createNestedObject(); x["mv"]=p.inputMv[i]; x["volts"]=p.inputMv[i]*.0053f; x["value"]=p.inputValue[i]; }
    auto m=d.createNestedObject("imu"); m["ok"]=imu.ok; m["ax"]=imu.ax; m["ay"]=imu.ay; m["az"]=imu.az; m["gx"]=imu.gx; m["gy"]=imu.gy; m["gz"]=imu.gz; m["temp"]=imu.temp; m["lateralG"]=imuLateralG(); m["yaw"]=imuYawRate();
    d["otaConfirmed"]=bootConfirmed;
}
String hubStatusJson() { return l8proto::statusJson(); }
static void send(int code,const String& data) { server.sendHeader("Cache-Control","no-store"); server.send(code,"application/json",data); }
static void error(int code,const String& text) { DynamicJsonDocument d(512); d["error"]=text; String out; serializeJson(d,out); send(code,out); }
static bool authorized() { if(server.header("X-L8-Token")==token) return true; error(403,"Session expired; reload the page"); return false; }
static bool otaBusy() { return updating || uploadOk || rebootAt!=0; }
static bool powerReady() { auto p=powerSnapshot(); return p.ioOk && !p.ioLatched; }
static void installProtocol() {
    l8proto::Hooks h;
    h.bootId=[]{ return bootId.c_str(); };
    h.otaBusy=otaBusy; h.powerReady=powerReady;
    h.masterChanged=[]{ powerTick(); };
    h.resetLatch=[](int ch){ powerResetLatch(ch); };
    h.imuOk=[]{ return imu.ok; };
    h.calibrate=[]{ imuCalibrate(); };
    h.reconnect=[]{ reconnectAt=millis()+500; };
    h.reboot=[]{ powerTick(); powerInhibit(true); settingsSaveNow(); rebootAt=millis()+500; };
    h.info=infoFill; h.status=statusFill;
    l8proto::setHooks(h);
}
static void legacyRoute(const char* route) {
    if(!authorized()) return;
    const String& body=server.arg("plain"); int http=500;
    String out=l8proto::legacyHttp(route,body.c_str(),body.length(),http); send(http,out);
}
static void upload() {
    auto& u=server.upload();
    if(u.status==UPLOAD_FILE_START) {
        uploadOk=false; uploadError="";
        if(settingsPreviewActive()) { uploadError="Save or discard preview before OTA"; return; }
        if(server.header("X-L8-Token")!=token) { uploadError="Session expired"; return; }
        drive.masterOn=false; powerTick(); powerInhibit(true);
        // The independent safety task must confirm the inhibit before flash writes.
        uint32_t start=millis(); while(millis()-start<250 && !powerSnapshot().inhibited) delay(5);
        if(!powerSnapshot().inhibited || !powerSnapshot().ioOk || powerSnapshot().on) { uploadError="Could not confirm outputs off"; return; }
        ledsPause(); updating=true;
        if(!Update.begin(UPDATE_SIZE_UNKNOWN,U_FLASH)) uploadError=Update.errorString();
    } else if(u.status==UPLOAD_FILE_WRITE) {
        if(updating && !uploadError.length() && Update.write(u.buf,u.currentSize)!=u.currentSize) uploadError=Update.errorString();
    } else if(u.status==UPLOAD_FILE_END) {
        if(updating && !uploadError.length()) { uploadOk=Update.end(true); if(!uploadOk) uploadError=Update.errorString(); }
        else if(updating) Update.abort();
        if(updating) { updating=false; ledsResume(); } // Master stays off
    } else if(u.status==UPLOAD_FILE_ABORTED) {
        if(updating) { Update.abort(); updating=false; ledsResume(); }
        uploadError="Upload aborted"; syncHubInhibit();
    }
}
void setupHub() {
    uint8_t mac[6]={}; esp_efuse_mac_get_default(mac);
    char id[16]; snprintf(id,sizeof(id),"L8-%02X%02X%02X%02X%02X%02X",mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]); deviceId=id;
    // Wi-Fi is already started (netBegin), so esp_random() draws from the hardware RNG.
    char b[33]; snprintf(b,sizeof(b),"%08lx%08lx%08lx%08lx",(unsigned long)esp_random(),(unsigned long)esp_random(),(unsigned long)esp_random(),(unsigned long)esp_random()); bootId=b;
    char t[33]; snprintf(t,sizeof(t),"%08lx%08lx%08lx%08lx",(unsigned long)esp_random(),(unsigned long)esp_random(),(unsigned long)esp_random(),(unsigned long)esp_random()); token=t;
    const char* headers[]={"X-L8-Token"}; server.collectHeaders(headers,1);
    installProtocol();
    server.on("/",HTTP_GET,[] { server.sendHeader("Content-Encoding","gzip"); server.sendHeader("Cache-Control","no-cache"); server.send_P(200,"text/html",(const char*)L8_UI,L8_UI_SIZE); });
    server.on("/api/session",HTTP_GET,[] { send(200,"{\"token\":\""+token+"\"}"); });
    server.on("/api/status",HTTP_GET,[] { send(200,hubStatusJson()); });
    server.on("/api/info",HTTP_GET,[] { send(200,hubInfoJson()); });
    server.on("/api/events",HTTP_GET,[] {
        if(server.arg("token")!=token) { error(403,"Session expired"); return; }
        bool v1=server.hasArg("v");
        if(v1 && server.arg("v")!="1") { int http; send(400,l8proto::handleV1("{\"v\":0}",7,l8proto::Transport::Http,true,http)); return; }
        for(auto& s:streams) if(!s.client || !s.client.connected()) {
            s.client.stop(); s.client=server.detach(); s.client.setNoDelay(true);
            s.pending="HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nCache-Control: no-store\r\nConnection: keep-alive\r\nX-Accel-Buffering: no\r\n\r\nretry: 3000\n\n";
            s.offset=0; s.queuedAt=millis(); s.v1=v1; return;
        }
        error(503,"Three status viewers already connected");
    });
    server.on("/api/config",HTTP_GET,[] { bool backup=server.arg("backup")=="1"; if(backup&&!authorized()) return; send(200,settingsJson(backup,true)); });
    server.on("/api/config",HTTP_POST,[] { legacyRoute("/api/config"); });
    for(const char* path:{"/api/preview","/api/apply","/api/save","/api/discard","/api/touch","/api/action"}) server.on(path,HTTP_POST,[path] { legacyRoute(path); });
    // API v1. Every POST command needs the session token; GET /api/info stays public.
    server.on("/api/command",HTTP_POST,[] {
        const String& body=server.arg("plain"); int http=500;
        String out=l8proto::handleV1(body.c_str(),body.length(),l8proto::Transport::Http,server.header("X-L8-Token")==token,http);
        send(http,out);
    });
    server.on("/api/log",HTTP_GET,[] { server.send(200,"text/plain",sysLogTail(16000)); });
    server.on("/api/can",HTTP_GET,[] { server.send(200,"text/plain",sniffTableString()); });
    server.on("/api/ota",HTTP_POST,[] {
        if(!authorized()) return;
        if(uploadOk) { send(200,"{\"ok\":true,\"reboot\":true}"); rebootAt=millis()+800; }
        else { syncHubInhibit(); error(400,uploadError.length()?uploadError:"No firmware received"); }
    },upload);
    auto captive=[] { server.sendHeader("Location","http://"+WiFi.softAPIP().toString()+"/"); server.send(302,"text/plain",""); };
    for(const char* path:{"/generate_204","/gen_204","/hotspot-detect.html","/library/test/success.html","/connecttest.txt","/ncsi.txt"}) server.on(path,HTTP_GET,captive);
    server.onNotFound([] { if(server.uri().startsWith("/api/")) { error(404,"Not found"); return; } server.sendHeader("Location","/"); server.send(302,"text/plain",""); });
    server.begin(); Serial.println("L8 configurator ready; USB JSON: info / status / get_config / set_config / action");
}
static void serialTick() {
    int budget=2048;
    while(Serial.available() && budget--) {
        auto event=usbFramer.feed(char(Serial.read()));
        if(event==LineFramer::Overflow) Serial.println("L8 "+l8proto::usbOverflow());
        if(event!=LineFramer::Line) continue;
        const std::string& line=usbFramer.text();
        String out=l8proto::handleUsbLine(line.data(),line.size());
        usbFramer.release(); Serial.println("L8 "+out);
    }
}
void updateHub() {
    server.handleClient(); streamTick(); serialTick(); uint32_t now=millis();
    if(reconnectAt && int32_t(now-reconnectAt)>=0) { reconnectAt=0; netReconnect(); }
    if(rebootAt && int32_t(now-rebootAt)>=0) { sysFlush(); ESP.restart(); }
    if(!bootConfirmed && now>=5000) {
        bool healthy=ledsHealthy() && powerSnapshot().ioOk && imu.ok && psramFound() && settingsHealthy();
        if(healthy) { esp_ota_mark_app_valid_cancel_rollback(); bootConfirmed=true; syncHubInhibit(); Serial.println("L8 boot self-check passed; OTA image confirmed"); }
        else if(now>=15000) { esp_ota_img_states_t st; const auto* p=esp_ota_get_running_partition(); if(esp_ota_get_state_partition(p,&st)==ESP_OK && st==ESP_OTA_IMG_PENDING_VERIFY) { sysFlush(); esp_ota_mark_app_invalid_rollback_and_reboot(); } }
    }
}
