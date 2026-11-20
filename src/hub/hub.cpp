#include "hub.h"
#include <WebServer.h>
#include <WiFi.h>
#include <Update.h>
#include <LittleFS.h>
#include <esp_ota_ops.h>
#include <esp32s3/spiram.h>
#include "settings/settings.h"
#include "net/net.h"
#include "power/power.h"
#include "imu/imu.h"
#include "vehicle/vehicle.h"
#include "system/system.h"
#include "board/board.h"
#include <channel_map.h>
#include "l8_ui.h"
static WebServer server(80);
static String token, serialLine;
static uint32_t rebootAt=0, reconnectAt=0;
static bool updating=false, uploadOk=false;
static String uploadError;
static bool bootConfirmed=false;
String hubStatusJson() {
    auto p=powerSnapshot(); DynamicJsonDocument d(8192);
    d["product"]="ZVER L8"; d["hardware"]=3; d["firmware"]=L8_FW_VERSION; d["uptimeMs"]=millis();
    d["master"]=drive.masterOn; d["vbat"]=p.vbat; d["ign"]=p.ign; d["hold"]=p.hold;
    d["allowWithoutIgn"]=savedData.allowWithoutIgn; d["ledPowerAllowed"]=p.ledPowerAllowed;
    d["ioOk"]=p.ioOk; d["ioFault"]=p.ioLatched; d["powerStatus"]=powerStatusString();
    d["totalAmps"]=p.ledAmps; d["sensorOn"]=p.sensorOn; d["sensorFault"]=p.sensorFault;
    d["heap"]=ESP.getFreeHeap(); d["psram"]=psramFound()?esp_spiram_get_size():0; d["flash"]=ESP.getFlashChipSize();
    d["network"]=netStatusString(); d["ap"]=netApOn(); d["apSsid"]=AP_SSID;
    d["ip"]=WiFi.status()==WL_CONNECTED?WiFi.localIP().toString():WiFi.softAPIP().toString();
    d["canStatus"]=canStatusString(); d["canBitrate"]=canBitrateString(); d["dataFresh"]=vehicleDataFresh();
    d["rpm"]=vehicle.rpm; d["speed"]=vehicle.speed; d["temp"]=vehicle.temp; d["throttle"]=vehicle.throttle;
    auto channels=d.createNestedArray("channels");
    for(int i=0;i<8;++i) { auto c=channels.createNestedObject(); c["on"]=p.channels[i].on; c["amps"]=p.channels[i].amps; c["fault"]=powerFaultName(p.channels[i].fault); c["isVolts"]=p.channels[i].isVolts; c["data"]=bool(boardDataMask()&v3::channelMask(i)); c["legacyIndex"]=v3::hardwareChannel(i); auto pins=c.createNestedArray("pins"); pins.add(v3::CHANNELS[i].power); pins.add(v3::CHANNELS[i].data); pins.add(v3::CHANNELS[i].ground); }
    auto inputs=d.createNestedArray("inputs"); for(int i=0;i<3;++i) { auto x=inputs.createNestedObject(); x["mv"]=p.inputMv[i]; x["volts"]=p.inputMv[i]*.0053f; x["value"]=p.inputValue[i]; }
    auto m=d.createNestedObject("imu"); m["ok"]=imu.ok; m["ax"]=imu.ax; m["ay"]=imu.ay; m["az"]=imu.az; m["gx"]=imu.gx; m["gy"]=imu.gy; m["gz"]=imu.gz; m["temp"]=imu.temp; m["lateralG"]=imuLateralG(); m["yaw"]=imuYawRate();
    d["otaConfirmed"]=bootConfirmed;
    String out; serializeJson(d,out); return out;
}
static bool action(JsonVariantConst j,String& error) {
    const char* cmd=j["action"]|"";
    if(!strcmp(cmd,"master")) {
        if(!j["on"].is<bool>()) { error="Expected on:boolean"; return false; }
        bool on=j["on"]; auto p=powerSnapshot();
        if(on && (!p.ioOk || p.ioLatched || updating)) { error="Power controller unavailable"; return false; }
        drive.masterOn=on; powerTick(); return true;
    }
    if(!strcmp(cmd,"reset")) { if(drive.masterOn) { error="Turn Master off before reset"; return false; } if(!j["channel"].is<int>()||j["channel"].as<int>() < -1 || j["channel"].as<int>()>7) { error="Expected channel -1..7"; return false; } powerResetLatch(j["channel"]); return true; }
    if(!strcmp(cmd,"calibrate")) { if(!imu.ok) { error="IMU unavailable"; return false; } imuCalibrate(); settingsSave(); return true; }
    if(!strcmp(cmd,"reconnect")) { reconnectAt=millis()+500; return true; }
    if(!strcmp(cmd,"reboot")) { drive.masterOn=false; powerTick(); powerInhibit(true); settingsSaveNow(); rebootAt=millis()+500; return true; }
    if(!strcmp(cmd,"preset_save")||!strcmp(cmd,"preset_load")) {
        if(!j["slot"].is<int>()||j["slot"].as<int>()<0||j["slot"].as<int>()>=NUM_PRESETS) { error="Expected slot 0..2"; return false; }
        if(!strcmp(cmd,"preset_save")) settingsPresetSave(j["slot"]);
        else if(!settingsPresetLoad(j["slot"])) { error="Empty preset"; return false; } return true;
    }
    error="Unknown action"; return false;
}
static bool apply(JsonVariantConst config,String& error) {
    if(updating) { error="OTA in progress"; return false; }
    int previousMode=canMode, previousBitrate=canBitrateSel;
    if(!settingsApply(config,error)) return false;
    // Both setters restart CAN. A mode change already uses the new bitrate.
    if(canMode!=previousMode) vehicleSetMode(canMode);
    else if(canBitrateSel!=previousBitrate) vehicleSetBitrate(canBitrateSel);
    settingsSave(); return true;
}
static void send(int code,const String& data) { server.sendHeader("Cache-Control","no-store"); server.send(code,"application/json",data); }
static void error(int code,const String& text) { DynamicJsonDocument d(512); d["error"]=text; String out; serializeJson(d,out); send(code,out); }
static bool authorized() { if(server.header("X-L8-Token")==token) return true; error(403,"Session expired; reload the page"); return false; }
static bool body(DynamicJsonDocument& d) { if(server.arg("plain").length()>24000) { error(413,"Config is too large"); return false; } if(deserializeJson(d,server.arg("plain"))) { error(400,"Invalid JSON"); return false; } return true; }
static void upload() {
    auto& u=server.upload();
    if(u.status==UPLOAD_FILE_START) {
        uploadOk=false; uploadError="";
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
        uploadError="Upload aborted"; powerInhibit(false);
    }
}
void setupHub() {
    char t[33]; snprintf(t,sizeof(t),"%08lx%08lx%08lx%08lx",(unsigned long)esp_random(),(unsigned long)esp_random(),(unsigned long)esp_random(),(unsigned long)esp_random()); token=t;
    const char* headers[]={"X-L8-Token"}; server.collectHeaders(headers,1);
    server.on("/",HTTP_GET,[] { server.sendHeader("Content-Encoding","gzip"); server.sendHeader("Cache-Control","no-cache"); server.send_P(200,"text/html",(const char*)L8_UI,L8_UI_SIZE); });
    server.on("/api/session",HTTP_GET,[] { send(200,"{\"token\":\""+token+"\"}"); });
    server.on("/api/status",HTTP_GET,[] { send(200,hubStatusJson()); });
    server.on("/api/config",HTTP_GET,[] { bool backup=server.arg("backup")=="1"; if(backup&&!authorized()) return; send(200,settingsJson(backup)); });
    server.on("/api/config",HTTP_POST,[] { if(!authorized()) return; DynamicJsonDocument d(24576); if(!body(d)) return; String e; if(!apply(d.as<JsonVariantConst>(),e)) { error(400,e); return; } send(200,String("{\"ok\":true,\"master\":")+(drive.masterOn?"true}":"false}")); });
    server.on("/api/action",HTTP_POST,[] { if(!authorized()) return; DynamicJsonDocument d(1024); if(!body(d)) return; String e; if(!action(d.as<JsonVariantConst>(),e)) { error(400,e); return; } send(200,"{\"ok\":true}"); });
    server.on("/api/log",HTTP_GET,[] { server.send(200,"text/plain",sysLogTail(16000)); });
    server.on("/api/can",HTTP_GET,[] { server.send(200,"text/plain",sniffTableString()); });
    server.on("/api/ota",HTTP_POST,[] {
        if(!authorized()) return;
        if(uploadOk) { send(200,"{\"ok\":true,\"reboot\":true}"); rebootAt=millis()+800; }
        else { powerInhibit(false); error(400,uploadError.length()?uploadError:"No firmware received"); }
    },upload);
    server.onNotFound([] { if(server.uri().startsWith("/api/")) { error(404,"Not found"); return; } server.sendHeader("Location","/"); server.send(302,"text/plain",""); });
    server.begin(); Serial.println("L8 configurator ready; USB JSON: status / get_config / set_config / action");
}
static void serialTick() {
    static bool discardLine=false;
    int budget=2048;
    while(Serial.available() && budget--) {
        char ch=Serial.read(); if(ch=='\r') continue;
        if(discardLine) { if(ch=='\n') discardLine=false; continue; }
        if(ch!='\n') { if(serialLine.length()<24000) serialLine+=ch; else { serialLine=""; discardLine=true; Serial.println("L8 {\"error\":\"line too long\"}"); } continue; }
        if(!serialLine.length()) continue;
        DynamicJsonDocument d(24576); String out;
        if(deserializeJson(d,serialLine)) out="{\"error\":\"Invalid JSON\"}";
        else {
            const char* cmd=d["cmd"]|"";
            if(!strcmp(cmd,"status")) out=hubStatusJson();
            else if(!strcmp(cmd,"get_config")) out=settingsJson();
            else {
                String e; bool ok=!strcmp(cmd,"set_config")?apply(d["config"],e):!strcmp(cmd,"action")?action(d.as<JsonVariantConst>(),e):false;
                DynamicJsonDocument result(512); result["ok"]=ok; if(!ok) result["error"]=e.length()?e:"Unknown command"; serializeJson(result,out);
            }
        }
        serialLine=""; Serial.println("L8 "+out);
    }
}
void updateHub() {
    server.handleClient(); serialTick(); uint32_t now=millis();
    if(reconnectAt && int32_t(now-reconnectAt)>=0) { reconnectAt=0; netReconnect(); }
    if(rebootAt && int32_t(now-rebootAt)>=0) ESP.restart();
    if(!bootConfirmed && now>=5000) {
        bool healthy=powerSnapshot().ioOk && imu.ok && psramFound() && settingsHealthy();
        if(healthy) { esp_ota_mark_app_valid_cancel_rollback(); bootConfirmed=true; powerInhibit(false); Serial.println("L8 boot self-check passed; OTA image confirmed"); }
        else if(now>=15000) { esp_ota_img_states_t st; const auto* p=esp_ota_get_running_partition(); if(esp_ota_get_state_partition(p,&st)==ESP_OK && st==ESP_OTA_IMG_PENDING_VERIFY) esp_ota_mark_app_invalid_rollback_and_reboot(); }
    }
}
