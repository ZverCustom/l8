#include "settings.h"
#include <LittleFS.h>
#include "net/net.h"
#include "vehicle/vehicle.h"
#include "power/power.h"
SavedData savedData;
DriveSettings& drive=savedData.drive;
static bool healthy=false, pending=false;
static uint32_t requested=0;
static constexpr const char* PATH="/config-v3.json";
static void defaults() {
    memset(&savedData,0,sizeof(savedData));
    savedData.allowWithoutIgn=true;
    savedData.can_setting={2000,7000,60,30};
    savedData.canMode=CAN_MODE_SNIFFER;
    savedData.bitrate=1; // fixed 500 kbit/s; 0 selects passive auto scan
    for(int i=0;i<8;++i) {
        savedData.numLeds[i]=60;
        auto& s=savedData.stripSettings[i]; s.enabled=true; s.brightness=100;
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
    o["color"]=s.color; o["brightness"]=s.brightness; o["count"]=count; o["order"]=s.colorOrder;
    o["source"]=s.scaleSource; o["middle"]=s.bottom; o["center"]=s.center;
}
static void encodeCan(JsonObject o,const CanSettings& c) { o["minRpm"]=c.minRPM; o["maxRpm"]=c.maxRPM; o["speed"]=c.speed; o["brightness"]=c.brightess; }
String settingsJson(bool secrets) {
    pullLive(); DynamicJsonDocument doc(24576); doc["schema"]=3;
    auto g=doc.createNestedObject("global"); encodeCan(g,can_setting);
    g["autoStart"]=savedData.autoStart; g["allowWithoutIgn"]=savedData.allowWithoutIgn; g["sensor5v"]=savedData.sensor5v;
    g["shiftRpm"]=drive.shiftRPM; g["shiftColor"]=drive.shiftColor; g["engineOff"]=drive.engineOff;
    g["dim"]=drive.engineOffDim; g["powerLimitMa"]=drive.powerLimitMa; g["maxSpeed"]=drive.maxSpeed;
    g["maxG"]=drive.maxLateralG; g["maxYaw"]=drive.maxYawRate; g["lateralAxis"]=drive.lateralAxis; g["yawAxis"]=drive.yawAxis;
    g["cutPowerEngineOff"]=savedData.cutPowerEngineOff;
    auto acc=g.createNestedArray("accOffset"), gyro=g.createNestedArray("gyroOffset");
    for(int i=0;i<3;++i) { acc.add(drive.accOff[i]); gyro.add(drive.gyrOff[i]); }
    auto strips=doc.createNestedArray("strips");
    for(int i=0;i<8;++i) { auto o=strips.createNestedObject(); encodeStrip(o,stripSettings[i],numLedsPerStrip[i]); o["maxAmps"]=savedData.channelMaxAmps[i]; o["currentCal"]=savedData.channelCal[i]; }
    auto inputs=doc.createNestedArray("inputs");
    for(auto& i:savedData.inputs) { auto o=inputs.createNestedObject(); o["mode"]=i.mode; o["low"]=i.low; o["high"]=i.high; o["thresholdMv"]=i.thresholdMv; }
    auto can=doc.createNestedObject("can"); can["mode"]=canMode; can["bitrate"]=canBitrateSel;
    auto net=doc.createNestedObject("network"); net["ssid"]=wifiSsid; net["password"]=secrets?wifiPass:""; net["keepPassword"]=!secrets;
    auto presets=doc.createNestedArray("presets");
    for(auto& p:savedData.presets) { auto o=presets.createNestedObject(); o["used"]=p.used; o["name"]=p.name; if(!p.used) continue; encodeCan(o.createNestedObject("global"),p.can); auto a=o.createNestedArray("strips"); for(int i=0;i<8;++i) encodeStrip(a.createNestedObject(),p.strips[i],p.numLeds[i]); }
    String out; serializeJson(doc,out); return out;
}
static bool number(JsonVariantConst v,float lo,float hi) { return v.is<float>() && isfinite(v.as<float>()) && v.as<float>()>=lo && v.as<float>()<=hi; }
static bool integer(JsonVariantConst v,int lo,int hi) { return v.is<int>() && v.as<int>()>=lo && v.as<int>()<=hi; }
static bool decodeCan(JsonObjectConst o,CanSettings& c) {
    if(!integer(o["minRpm"],0,12000)||!integer(o["maxRpm"],1,16000)||o["maxRpm"].as<int>()<=o["minRpm"].as<int>()||!integer(o["speed"],0,100)||!integer(o["brightness"],0,100)) return false;
    c={o["minRpm"],o["maxRpm"],o["speed"],o["brightness"]}; return true;
}
static bool decodeStrip(JsonObjectConst o,StripSettings& s,int& count) {
    if(!o["enabled"].is<bool>()||!o["reverse"].is<bool>()||!o["middle"].is<bool>()||!integer(o["mode"],0,2)||!integer(o["animation"],0,15)||!integer(o["color"],0,0xffffff)||!integer(o["brightness"],0,100)||!integer(o["count"],MIN_LEDS,MAX_LEDS)||!integer(o["order"],0,5)||!integer(o["source"],0,7)||!integer(o["center"],0,MAX_LEDS-1)||o["center"].as<int>()>=o["count"].as<int>()) return false;
    s.enabled=o["enabled"]; s.reverse=o["reverse"]; s.bottom=o["middle"]; s.mode=o["mode"]; s.animationIndex=o["animation"];
    s.color=o["color"]; s.brightness=o["brightness"]; count=o["count"]; s.colorOrder=o["order"]; s.scaleSource=o["source"]; s.center=o["center"]; return true;
}
bool settingsApply(JsonVariantConst value,String& error) {
    auto o=value.as<JsonObjectConst>(); auto g=o["global"].as<JsonObjectConst>();
    SavedData next=savedData;
    auto fail=[&](const char* text) { error=text; return false; };
    if(!integer(o["schema"],3,3)) return fail("Expected config schema 3");
    if(!decodeCan(g,next.can_setting)) return fail("Invalid global RPM/brightness/speed");
    for(auto key:{"autoStart","allowWithoutIgn","sensor5v","cutPowerEngineOff"}) if(!g[key].is<bool>()) return fail("Invalid global switch");
    if(!integer(g["shiftRpm"],0,16000)||!integer(g["shiftColor"],0,0xffffff)||!integer(g["engineOff"],0,2)||!integer(g["dim"],0,100)||!integer(g["powerLimitMa"],0,40000)||!integer(g["maxSpeed"],1,400)||!number(g["maxG"],.1,10)||!number(g["maxYaw"],1,2000)||!integer(g["lateralAxis"],0,5)||!integer(g["yawAxis"],0,5)) return fail("Invalid global setting");
    next.autoStart=g["autoStart"]; next.allowWithoutIgn=g["allowWithoutIgn"]; next.sensor5v=g["sensor5v"]; next.cutPowerEngineOff=g["cutPowerEngineOff"];
    auto& d=next.drive; d.masterOn=false; d.shiftRPM=g["shiftRpm"]; d.shiftColor=g["shiftColor"]; d.engineOff=g["engineOff"]; d.engineOffDim=g["dim"]; d.powerLimitMa=g["powerLimitMa"];
    d.maxSpeed=g["maxSpeed"]; d.maxLateralG=g["maxG"]; d.maxYawRate=g["maxYaw"]; d.lateralAxis=g["lateralAxis"]; d.yawAxis=g["yawAxis"];
    for(auto key:{"accOffset","gyroOffset"}) if(!g[key].is<JsonArrayConst>() || g[key].size()!=3) return fail("Invalid IMU offsets");
    for(int i=0;i<3;++i) { if(!number(g["accOffset"][i],-4,4)||!number(g["gyroOffset"][i],-500,500)) return fail("Invalid IMU offset range"); d.accOff[i]=g["accOffset"][i]; d.gyrOff[i]=g["gyroOffset"][i]; }
    if(!o["strips"].is<JsonArrayConst>()||o["strips"].size()!=8) return fail("Expected eight strips");
    for(int i=0;i<8;++i) { auto s=o["strips"][i].as<JsonObjectConst>(); if(!decodeStrip(s,next.stripSettings[i],next.numLeds[i])||!number(s["maxAmps"],.5,5)||!number(s["currentCal"],.3,3)) return fail("Invalid strip setting (8..1000 LEDs, 0.5..5 A)"); next.channelMaxAmps[i]=s["maxAmps"]; next.channelCal[i]=s["currentCal"]; }
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
        for(int j=0;j<8;++j) if(!decodeStrip(p["strips"][j],next.presets[i].strips[j],next.presets[i].numLeds[j])) return fail("Invalid preset strip");
    }
    // Commit only after validating every field; import always disarms outputs.
    drive.masterOn=false; powerTick(); powerInhibit(true); ledsPause();
    savedData=next; pushLive(); requestLedReinit(); ledsResume(); powerTick(); powerInhibit(false);
    return true;
}
static bool readConfig(const char* path) {
    File file=LittleFS.open(path,"r"); if(!file||file.size()>24000) return false;
    DynamicJsonDocument doc(24576); auto result=deserializeJson(doc,file); file.close();
    String error; return !result && settingsApply(doc.as<JsonVariantConst>(),error);
}
void settingsBegin() {
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
void settingsSave() { pullLive(); pending=true; requested=millis(); }
void settingsSaveNow() {
    if(!healthy) return;
    String data=settingsJson(true);
    ledsPause();
    File f=LittleFS.open("/config-v3.tmp","w");
    bool ok=f && f.print(data)==data.length(); if(f) { f.flush(); f.close(); }
    if(ok) {
        LittleFS.remove("/config-v3.bak");
        if(LittleFS.exists(PATH)) ok=LittleFS.rename(PATH,"/config-v3.bak");
        if(ok) ok=LittleFS.rename("/config-v3.tmp",PATH);
    }
    ledsResume(); pending=!ok; if(!ok) Serial.println("Config save failed (previous config retained)");
}
void settingsTick() { if(pending && millis()-requested>=1500) settingsSaveNow(); }
void settingsPresetSave(int i) {
    if(i<0||i>=NUM_PRESETS) return; auto& p=savedData.presets[i]; p.used=true;
    if(!p.name[0]) snprintf(p.name,sizeof(p.name),"Preset %d",i+1);
    for(int s=0;s<8;++s) { p.strips[s]=stripSettings[s]; p.numLeds[s]=numLedsPerStrip[s]; } p.can=can_setting; settingsSave();
}
bool settingsPresetLoad(int i) {
    if(i<0||i>=NUM_PRESETS||!savedData.presets[i].used) return false;
    drive.masterOn=false; powerTick(); powerInhibit(true); ledsPause(); auto& p=savedData.presets[i];
    for(int s=0;s<8;++s) { stripSettings[s]=p.strips[s]; numLedsPerStrip[s]=p.numLeds[s]; }
    can_setting=p.can; requestLedReinit(); ledsResume(); powerInhibit(false); settingsSave(); return true;
}
String settingsPresetName(int i) { return i>=0&&i<NUM_PRESETS&&savedData.presets[i].used ? savedData.presets[i].name : "empty"; }
