#include "settings.h"
#include <LittleFS.h>
#include "net/net.h"
#include "vehicle/vehicle.h"
#include "power/power.h"
#include <channel_map.h>
SavedData savedData;
DriveSettings& drive=savedData.drive;
static bool healthy=false, pending=false;
static uint32_t requested=0;
static String pendingJson;
static SavedData previewBase;
static String previewOwner;
static uint32_t revision=0, previewTouched=0;
static bool previewActive=false;
static constexpr const char* PATH="/config-v3.json";
static void defaults() {
    memset(&savedData,0,sizeof(savedData));
    savedData.allowWithoutIgn=true; savedData.autoRetry=true;
    savedData.can_setting={2000,7000,60,30};
    savedData.canMode=CAN_MODE_SNIFFER;
    savedData.bitrate=1; // fixed 500 kbit/s; 0 selects passive auto scan
    for(int i=0;i<8;++i) {
        savedData.numLeds[i]=60;
        auto& s=savedData.stripSettings[i]; s.enabled=true; s.brightness=100; s.speed=60;
        s.color=0xFF2400; s.colorOrder=1;
        savedData.channelMaxAmps[i]=3.f; savedData.channelCal[i]=1.f;
    }
    for(auto& input:savedData.inputs) { input.mode=0; input.low=0; input.high=5; input.thresholdMv=600; }
    auto& d=drive; d.masterOn=false; d.shiftColor=0xFF0000; d.engineOffDim=20;
    d.lateralAxis=2; d.yawAxis=4; d.maxSpeed=200; d.maxLateralG=1; d.maxYawRate=60; d.ledMaxAmps=5;
    savedData.currentCal=1;
    strlcpy(savedData.wifiSsid,netDefaultSsid(),sizeof(savedData.wifiSsid));
    strlcpy(savedData.wifiPass,netDefaultPass(),sizeof(savedData.wifiPass));
}
static void pushLive() {
    can_setting=savedData.can_setting;
    for(int i=0;i<8;++i) { stripSettings[i]=savedData.stripSettings[i]; numLedsPerStrip[i]=savedData.numLeds[i]; }
    canMode=savedData.canMode; canBitrateSel=savedData.bitrate;
    wifiSsid=savedData.wifiSsid; wifiPass=savedData.wifiPass;
}
static void pullLive() {
    savedData.can_setting=can_setting; savedData.canMode=canMode; savedData.bitrate=canBitrateSel;
    for(int i=0;i<8;++i) { savedData.stripSettings[i]=stripSettings[i]; savedData.numLeds[i]=numLedsPerStrip[i]; }
    strlcpy(savedData.wifiSsid,wifiSsid.c_str(),sizeof(savedData.wifiSsid));
    strlcpy(savedData.wifiPass,wifiPass.c_str(),sizeof(savedData.wifiPass));
}
static void encodeStrip(JsonObject o,const StripSettings& s,int count) {
    o["enabled"]=s.enabled; o["reverse"]=s.reverse; o["mode"]=s.mode; o["animation"]=s.animationIndex;
    o["speed"]=s.speed; o["color"]=s.color; o["brightness"]=s.brightness; o["count"]=count; o["order"]=s.colorOrder;
    o["source"]=s.scaleSource; o["middle"]=s.bottom; o["center"]=s.center;
}
static void encodeCan(JsonObject o,const CanSettings& c) { o["minRpm"]=c.minRPM; o["maxRpm"]=c.maxRPM; o["speed"]=c.speed; o["brightness"]=c.brightess; }
String settingsJson(bool secrets,bool baseline) {
    pullLive(); const SavedData& data=((secrets || baseline) && previewActive)?previewBase:savedData; DynamicJsonDocument doc(24576); doc["schema"]=L8_CONFIG_SCHEMA;
    auto g=doc.createNestedObject("global"); encodeCan(g,data.can_setting);
    g["autoRetry"]=data.autoRetry; g["autoStart"]=data.autoStart; g["allowWithoutIgn"]=data.allowWithoutIgn; g["sensor5v"]=data.sensor5v;
    g["shiftRpm"]=data.drive.shiftRPM; g["shiftColor"]=data.drive.shiftColor; g["engineOff"]=data.drive.engineOff;
    g["dim"]=data.drive.engineOffDim; g["powerLimitMa"]=data.drive.powerLimitMa; g["maxSpeed"]=data.drive.maxSpeed;
    g["maxG"]=data.drive.maxLateralG; g["maxYaw"]=data.drive.maxYawRate; g["lateralAxis"]=data.drive.lateralAxis; g["yawAxis"]=data.drive.yawAxis;
    g["cutPowerEngineOff"]=data.cutPowerEngineOff;
    auto acc=g.createNestedArray("accOffset"), gyro=g.createNestedArray("gyroOffset");
    for(int i=0;i<3;++i) { acc.add(data.drive.accOff[i]); gyro.add(data.drive.gyrOff[i]); }
    auto strips=doc.createNestedArray("strips");
    for(int i=0;i<8;++i) { auto o=strips.createNestedObject(); encodeStrip(o,data.stripSettings[i],data.numLeds[i]); o["maxAmps"]=data.channelMaxAmps[i]; o["currentCal"]=data.channelCal[i]; }
    auto inputs=doc.createNestedArray("inputs");
    for(auto& i:data.inputs) { auto o=inputs.createNestedObject(); o["mode"]=i.mode; o["low"]=i.low; o["high"]=i.high; o["thresholdMv"]=i.thresholdMv; }
    auto can=doc.createNestedObject("can"); can["mode"]=data.canMode; can["bitrate"]=data.bitrate;
    auto net=doc.createNestedObject("network"); net["ssid"]=data.wifiSsid; net["password"]=secrets?data.wifiPass:""; net["keepPassword"]=!secrets;
    auto presets=doc.createNestedArray("presets");
    for(auto& p:data.presets) { auto o=presets.createNestedObject(); o["used"]=p.used; o["name"]=p.name; if(!p.used) continue; encodeCan(o.createNestedObject("global"),p.can); auto a=o.createNestedArray("strips"); for(int i=0;i<8;++i) encodeStrip(a.createNestedObject(),p.strips[i],p.numLeds[i]); }
    String out; serializeJson(doc,out); return out;
}
static bool number(JsonVariantConst v,float lo,float hi) { return v.is<float>() && isfinite(v.as<float>()) && v.as<float>()>=lo && v.as<float>()<=hi; }
static bool integer(JsonVariantConst v,int lo,int hi) { return v.is<int>() && v.as<int>()>=lo && v.as<int>()<=hi; }
static bool decodeCan(JsonObjectConst o,CanSettings& c) {
    if(!integer(o["minRpm"],0,12000)||!integer(o["maxRpm"],1,16000)||o["maxRpm"].as<int>()<=o["minRpm"].as<int>()||!integer(o["speed"],0,100)||!integer(o["brightness"],0,100)) return false;
    c={o["minRpm"],o["maxRpm"],o["speed"],o["brightness"]}; return true;
}
static bool decodeStrip(JsonObjectConst o,StripSettings& s,int& count,int legacySpeed=60,int schema=5) {
    if(!o["enabled"].is<bool>()||!o["reverse"].is<bool>()||!o["middle"].is<bool>()||!integer(o["mode"],0,2)||!integer(o["animation"],0,15)||!integer(o["color"],0,0xffffff)||!integer(o["brightness"],0,100)||!integer(o["count"],MIN_LEDS,MAX_LEDS)||!integer(o["order"],0,5)||!integer(o["source"],0,7)||!integer(o["center"],0,MAX_LEDS-1)||o["center"].as<int>()>=o["count"].as<int>()) return false;
    if(schema>=5 && !integer(o["speed"],0,100)) return false;
    s.speed=schema>=5 ? o["speed"].as<int>() : legacySpeed;
    s.enabled=o["enabled"]; s.reverse=o["reverse"]; s.bottom=o["middle"]; s.mode=o["mode"]; s.animationIndex=o["animation"];
    s.color=o["color"]; s.brightness=o["brightness"]; count=o["count"]; s.colorOrder=o["order"]; s.scaleSource=o["source"]; s.center=o["center"]; return true;
}
bool settingsApply(JsonVariantConst value,String& error) {
    auto o=value.as<JsonObjectConst>(); auto g=o["global"].as<JsonObjectConst>();
    SavedData next=savedData;
    auto fail=[&](const char* text) { error=text; return false; };
    if(!integer(o["schema"],3,5)) return fail("Expected config schema 3, 4 or 5");
    if(!decodeCan(g,next.can_setting)) return fail("Invalid global RPM/brightness/speed");
    for(auto key:{"autoStart","allowWithoutIgn","sensor5v","cutPowerEngineOff"}) if(!g[key].is<bool>()) return fail("Invalid global switch");
    if(!integer(g["shiftRpm"],0,16000)||!integer(g["shiftColor"],0,0xffffff)||!integer(g["engineOff"],0,2)||!integer(g["dim"],0,100)||!integer(g["powerLimitMa"],0,40000)||!integer(g["maxSpeed"],1,400)||!number(g["maxG"],.1,10)||!number(g["maxYaw"],1,2000)||!integer(g["lateralAxis"],0,5)||!integer(g["yawAxis"],0,5)) return fail("Invalid global setting");
    if(o["schema"].as<int>()>=5 && !g["autoRetry"].is<bool>()) return fail("Invalid autoRetry switch");
    next.autoRetry=o["schema"].as<int>()>=5 ? g["autoRetry"].as<bool>() : true;
    next.autoStart=g["autoStart"]; next.allowWithoutIgn=g["allowWithoutIgn"]; next.sensor5v=g["sensor5v"]; next.cutPowerEngineOff=g["cutPowerEngineOff"];
    auto& d=next.drive; d.shiftRPM=g["shiftRpm"]; d.shiftColor=g["shiftColor"]; d.engineOff=g["engineOff"]; d.engineOffDim=g["dim"]; d.powerLimitMa=g["powerLimitMa"];
    d.maxSpeed=g["maxSpeed"]; d.maxLateralG=g["maxG"]; d.maxYawRate=g["maxYaw"]; d.lateralAxis=g["lateralAxis"]; d.yawAxis=g["yawAxis"];
    for(auto key:{"accOffset","gyroOffset"}) if(!g[key].is<JsonArrayConst>() || g[key].size()!=3) return fail("Invalid IMU offsets");
    for(int i=0;i<3;++i) { if(!number(g["accOffset"][i],-4,4)||!number(g["gyroOffset"][i],-500,500)) return fail("Invalid IMU offset range"); d.accOff[i]=g["accOffset"][i]; d.gyrOff[i]=g["gyroOffset"][i]; }
    if(!o["strips"].is<JsonArrayConst>()||o["strips"].size()!=8) return fail("Expected eight strips");
    for(int i=0;i<8;++i) { auto s=o["strips"][i].as<JsonObjectConst>(); if(!decodeStrip(s,next.stripSettings[i],next.numLeds[i],next.can_setting.speed,o["schema"])||!number(s["maxAmps"],.5,L8_MAX_CHANNEL_AMPS)||!number(s["currentCal"],.3,3)) return fail("Invalid strip setting (8..1000 LEDs, 0.5..5 A)"); next.channelMaxAmps[i]=s["maxAmps"]; next.channelCal[i]=s["currentCal"]; }
    if(!o["inputs"].is<JsonArrayConst>()||o["inputs"].size()!=3) return fail("Expected three inputs");
    for(int i=0;i<3;++i) { auto p=o["inputs"][i]; if(!integer(p["mode"],0,2)||!number(p["low"],0,16)||!number(p["high"],0,16)||p["high"].as<float>()<=p["low"].as<float>()||!number(p["thresholdMv"],0,3100)) return fail("Invalid input setting"); next.inputs[i]={p["mode"],p["low"],p["high"],p["thresholdMv"]}; }
    if(!integer(o["can"]["mode"],0,3)||!integer(o["can"]["bitrate"],0,5)) return fail("Invalid CAN setting");
    next.canMode=o["can"]["mode"]; next.bitrate=o["can"]["bitrate"];
    auto net=o["network"]; if(!net["ssid"].is<const char*>()||strlen(net["ssid"])>32||!net["password"].is<const char*>()||strlen(net["password"])>64||!net["keepPassword"].is<bool>()) return fail("Invalid Wi-Fi setting");
    strlcpy(next.wifiSsid,net["ssid"],sizeof(next.wifiSsid));
    if(!net["keepPassword"].as<bool>()) strlcpy(next.wifiPass,net["password"],sizeof(next.wifiPass));
    if(!o["presets"].is<JsonArrayConst>()||o["presets"].size()!=NUM_PRESETS) return fail("Expected three presets");
    for(int i=0;i<NUM_PRESETS;++i) {
        auto p=o["presets"][i]; if(!p["used"].is<bool>()||!p["name"].is<const char*>()||strlen(p["name"])>16) return fail("Invalid preset");
        next.presets[i].used=p["used"]; strlcpy(next.presets[i].name,p["name"],17); if(!next.presets[i].used) continue;
        if(!decodeCan(p["global"],next.presets[i].can)||!p["strips"].is<JsonArrayConst>()||p["strips"].size()!=8) return fail("Invalid preset contents");
        for(int j=0;j<8;++j) if(!decodeStrip(p["strips"][j],next.presets[i].strips[j],next.presets[i].numLeds[j],next.presets[i].can.speed,o["schema"])) return fail("Invalid preset strip");
    }
    // Schema 3 used schematic order. Preserve settings and protection limits on
    // their physical outputs when loading old files, imports or browser tabs.
    if(o["schema"].as<int>()==3) {
        v3::migrateLegacyChannels(next.stripSettings); v3::migrateLegacyChannels(next.numLeds);
        v3::migrateLegacyChannels(next.channelMaxAmps); v3::migrateLegacyChannels(next.channelCal);
        for(auto& p:next.presets) if(p.used) { v3::migrateLegacyChannels(p.strips); v3::migrateLegacyChannels(p.numLeds); }
    }
    // Commit between frames. Keep the runtime Master state and the safety task's
    // inhibit/latches intact; changing a colour must not cycle strip power.
    bool resize=false;
    for(int i=0;i<8;++i) resize |= next.numLeds[i]!=numLedsPerStrip[i];
    ledsPause();
    next.drive.masterOn=drive.masterOn;
    savedData=next; pushLive(); if(resize) requestLedReinit(); powerTick(); ledsResume(); ++revision;
    return true;
}
static bool readConfig(const char* path) {
    File file=LittleFS.open(path,"r"); if(!file||file.size()>L8_MAX_CONFIG_BYTES) return false;
    DynamicJsonDocument doc(24576); auto result=deserializeJson(doc,file); file.close();
    String error; bool ok=!result && settingsApply(doc.as<JsonVariantConst>(),error);
    if(ok && doc["schema"].as<int>()<5) settingsSave(); // write schema 5 atomically after startup
    return ok;
}
void settingsBegin() {
    pending=false; pendingJson=""; previewActive=false; previewOwner=""; revision=0;
    defaults(); pushLive();
    healthy=LittleFS.begin(true); // fresh board / v2 FS: config-v3 uses a separate file
    if(!healthy) { Serial.println("FS unavailable: defaults; outputs disarmed"); return; }
    bool exists=LittleFS.exists(PATH);
    if(exists && !readConfig(PATH)) { Serial.println("Config invalid; trying backup"); if(!readConfig("/config-v3.bak")) { defaults(); pushLive(); } }
    else if(!exists && LittleFS.exists("/config-v3.bak")) readConfig("/config-v3.bak");
    drive.masterOn=savedData.autoStart;
    Serial.println("L8 v3 settings ready");
}
bool settingsHealthy() { return healthy; }
void settingsSave() { ++revision; pendingJson=settingsJson(true); pending=true; requested=millis(); }
bool settingsSaveNow() {
    if(!healthy) return false;
    if(!pending) return true;
    const String& data=pendingJson;
    ledsFlashPause();
    File f=LittleFS.open("/config-v3.tmp","w");
    bool ok=f && f.print(data)==data.length(); if(f) { f.flush(); f.close(); }
    if(ok) {
        LittleFS.remove("/config-v3.bak");
        if(LittleFS.exists(PATH)) ok=LittleFS.rename(PATH,"/config-v3.bak");
        if(ok) ok=LittleFS.rename("/config-v3.tmp",PATH);
    }
    ledsFlashResume(); pending=!ok; if(ok) pendingJson=""; else Serial.println("Config save failed (previous config retained)");
    return ok;
}
static void restorePreview() {
    if(!previewActive) return;
    ledsPause(); previewBase.drive.masterOn=drive.masterOn;
    savedData=previewBase; pushLive(); requestLedReinit(); powerTick(); ledsResume();
    previewActive=false; previewOwner=""; ++revision;
}
void settingsTick() {
    if(previewActive && millis()-previewTouched>=L8_PREVIEW_LEASE_MS) {
        int mode=canMode, rate=canBitrateSel;
        restorePreview();
        if(mode!=canMode) vehicleSetMode(canMode); else if(rate!=canBitrateSel) vehicleSetBitrate(canBitrateSel);
    }
    if(pending && millis()-requested>=1500) { requested=millis(); settingsSaveNow(); }
}
bool settingsPreviewActive() { return previewActive; }
uint32_t settingsRevision() { return revision; }
static bool checkEditor(JsonVariantConst req,String& error,bool requireActive=false) {
    const char* owner=req["client"]|"";
    if(!*owner || strlen(owner)>48 || !req["revision"].is<uint32_t>()) { error="Invalid editor identity/revision"; return false; }
    if(req["revision"].as<uint32_t>()!=revision || (previewActive && previewOwner!=owner) || (requireActive && !previewActive)) { error="Configuration changed or another editor is active; reload"; return false; }
    return true;
}
bool settingsPreview(JsonVariantConst req,bool full,String& error) {
    if(!checkEditor(req,error)) return false;
    DynamicJsonDocument candidate(24576);
    if(full) candidate.set(req["config"]);
    else {
        if(!integer(req["strip"],0,7) || !req["patch"].is<JsonObjectConst>()) { error="Expected strip 0..7 and patch"; return false; }
        deserializeJson(candidate,settingsJson());
        auto dest=candidate["strips"][req["strip"].as<int>()].as<JsonObject>();
        for(JsonPairConst item:req["patch"].as<JsonObjectConst>()) {
            String key=item.key().c_str();
            if(key!="color" && key!="brightness" && key!="speed") { error="Preview patch accepts color, brightness and speed"; return false; }
            dest[key]=item.value();
        }
    }
    pullLive(); SavedData before=savedData;
    if(!settingsApply(candidate.as<JsonVariantConst>(),error)) return false;
    if(!previewActive) { previewBase=before; previewOwner=req["client"].as<const char*>(); previewActive=true; }
    previewTouched=millis(); return true;
}
bool settingsCommitPreview(JsonVariantConst req,String& error) {
    if(!checkEditor(req,error,true)) return false;
    // Commit only this checked revision. A previously scheduled write owns its
    // serialized snapshot, so it cannot accidentally persist later previews.
    bool oldPending=pending; String oldJson=pendingJson; uint32_t oldRequested=requested;
    previewActive=false; settingsSave();
    if(!settingsSaveNow()) { previewActive=true; pending=oldPending; pendingJson=oldJson; requested=oldRequested; --revision; error="Flash write failed; preview remains unsaved"; return false; }
    previewOwner=""; ++revision; return true;
}
bool settingsTouchPreview(JsonVariantConst req,String& error) {
    if(!checkEditor(req,error,true)) return false; previewTouched=millis(); return true;
}
bool settingsDiscardPreview(JsonVariantConst req,String& error) {
    if(!checkEditor(req,error)) return false;
    restorePreview(); return true;
}
void settingsPresetSave(int i) {
    if(i<0||i>=NUM_PRESETS) return; auto& p=savedData.presets[i]; p.used=true;
    if(!p.name[0]) snprintf(p.name,sizeof(p.name),"Preset %d",i+1);
    for(int s=0;s<8;++s) { p.strips[s]=stripSettings[s]; p.numLeds[s]=numLedsPerStrip[s]; } p.can=can_setting; settingsSave();
}
bool settingsPresetLoad(int i) {
    if(i<0||i>=NUM_PRESETS||!savedData.presets[i].used) return false;
    ledsPause(); auto& p=savedData.presets[i];
    for(int s=0;s<8;++s) { stripSettings[s]=p.strips[s]; numLedsPerStrip[s]=p.numLeds[s]; }
    can_setting=p.can; requestLedReinit(); powerTick(); ledsResume(); settingsSave(); return true;
}
String settingsPresetName(int i) { return i>=0&&i<NUM_PRESETS&&savedData.presets[i].used ? savedData.presets[i].name : "empty"; }
