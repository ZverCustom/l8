#include <unity.h>
#include <Arduino.h>
#include <LittleFS.h>
#include <string>
#include "../../src/settings/settings.cpp"
#include "../../src/protocol/protocol.cpp"
#include <line_framer.h>
#include <vector>
SerialStub Serial; uint32_t testMillis=0; FakeFS LittleFS;
CanSettings can_setting; StripSettings stripSettings[NUM_STRIPS]; int numLedsPerStrip[NUM_STRIPS];
int canMode=2,canBitrateSel=1; String wifiSsid,wifiPass;
const char* netDefaultSsid(){return "";}const char* netDefaultPass(){return "";}
int canRestarts=0;
void vehicleSetMode(int n){canMode=n;++canRestarts;}void vehicleSetBitrate(int n){canBitrateSel=n;++canRestarts;}
void ledsPause(){}void ledsResume(){}void ledsFlashPause(){}void ledsFlashResume(){}void requestLedReinit(){}void powerTick(){}
using namespace l8proto;
// ---- fake device ----
static std::string boot="00112233445566778899aabbccddeeff";
static bool ota=false, ready=true, imuGood=true;
static int masterPushes=0, resets=0, lastReset=99, calibrations=0, reconnects=0, reboots=0;
static void fakeStatus(JsonObject o){o["configRevision"]=settingsRevision();uint32_t s;if(settingsSavedRevision(s))o["savedRevision"]=s;else o["savedRevision"]=nullptr;o["bootId"]=boot.c_str();o["master"]=drive.masterOn;o["previewActive"]=settingsPreviewActive();}
static void fakeInfo(JsonObject o){o["product"]="ZVER L8";o["bootId"]=boot.c_str();}
static void install(){Hooks h;h.bootId=[]{return boot.c_str();};h.otaBusy=[]{return ota;};h.powerReady=[]{return ready;};
    h.masterChanged=[]{++masterPushes;};h.resetLatch=[](int c){++resets;lastReset=c;};h.imuOk=[]{return imuGood;};h.calibrate=[]{++calibrations;drive.accOff[0]=.5f;};
    h.reconnect=[]{++reconnects;};h.reboot=[]{++reboots;};h.info=fakeInfo;h.status=fakeStatus;setHooks(h);}
void setUp(){LittleFS.files.clear();LittleFS.failWrite=LittleFS.failInstall=LittleFS.failBackup=LittleFS.failBegin=LittleFS.shortWrite=false;LittleFS.writes=0;testMillis=0;
    settingsBegin();settingsSave();settingsSaveNow();boot="00112233445566778899aabbccddeeff";ota=false;ready=true;imuGood=true;drive.masterOn=false;
    masterPushes=resets=calibrations=reconnects=reboots=canRestarts=0;lastReset=99;install();}
void tearDown(){}
// ---- helpers ----
static std::string fill(std::string t){ // $B -> bootId, $R -> current revision
    for(size_t p;(p=t.find("$B"))!=std::string::npos;) t.replace(p,2,boot);
    for(size_t p;(p=t.find("$R"))!=std::string::npos;) t.replace(p,2,std::to_string(settingsRevision()));
    return t;
}
static DynamicJsonDocument reply(65536); static int lastHttp=0;
static JsonObject usb(const std::string& raw){String out=handleUsbLine(raw.data(),raw.size());reply.clear();TEST_ASSERT_FALSE(deserializeJson(reply,out.c_str()));return reply.as<JsonObject>();}
static JsonObject http(const std::string& raw,bool auth=true){String out=handleV1(raw.data(),raw.size(),Transport::Http,auth,lastHttp);reply.clear();TEST_ASSERT_FALSE(deserializeJson(reply,out.c_str()));return reply.as<JsonObject>();}
static std::string text(JsonVariantConst v){std::string s;serializeJson(v,s);return s;}
static void expectError(JsonObject r,const char* code){TEST_ASSERT_FALSE_MESSAGE(r["ok"].as<bool>(),text(r).c_str());TEST_ASSERT_EQUAL_STRING_MESSAGE(code,r["error"]["code"]|"",text(r).c_str());}
static void expectOk(JsonObject r){TEST_ASSERT_TRUE_MESSAGE(r["ok"].as<bool>(),text(r).c_str());}
static std::string state(){std::string s=settingsJson(true).c_str();return s+"|rev="+std::to_string(settingsRevision())+"|m="+std::to_string(drive.masterOn)+"|p="+std::to_string(settingsPreviewActive())+settingsPreviewOwner()+"|w="+std::to_string(LittleFS.writes);}
static std::string previewMsg(int color,const char* client="app"){return fill(std::string("{\"v\":1,\"id\":\"p\",\"cmd\":\"preview\",\"bootId\":\"$B\",\"client\":\"")+client+"\",\"revision\":$R,\"strip\":1,\"patch\":{\"color\":"+std::to_string(color)+"}}");}

// 1. every command: success, a significant failure, and HTTP/USB parity
void envelope_shape_and_reads(){
    auto r=usb(fill("{\"v\":1,\"id\":\"r1\",\"cmd\":\"info\"}"));expectOk(r);
    TEST_ASSERT_EQUAL(1,r["v"].as<int>());TEST_ASSERT_EQUAL_STRING("r1",r["id"]);TEST_ASSERT_EQUAL_STRING(boot.c_str(),r["bootId"]);
    TEST_ASSERT_EQUAL(settingsRevision(),r["revision"].as<uint32_t>());TEST_ASSERT_EQUAL(settingsRevision(),r["savedRevision"].as<uint32_t>());TEST_ASSERT_EQUAL_STRING("ZVER L8",r["result"]["product"]);
    r=usb("{\"v\":1,\"id\":\"r2\",\"cmd\":\"status\"}");expectOk(r);TEST_ASSERT_EQUAL(settingsRevision(),r["result"]["configRevision"].as<uint32_t>());
    r=usb("{\"v\":1,\"id\":\"r3\",\"cmd\":\"get_config\"}");expectOk(r);TEST_ASSERT_EQUAL_STRING("baseline",r["result"]["view"]);
    TEST_ASSERT_EQUAL(5,r["result"]["config"]["schema"].as<int>());TEST_ASSERT_EQUAL_STRING("",r["result"]["config"]["network"]["password"]);TEST_ASSERT_TRUE(r["result"]["config"]["network"]["keepPassword"]);
}
static void parity(const std::string& raw){
    std::string before=state();
    std::string u=text(usb(fill(raw)));std::string su=state();
    setUp();TEST_ASSERT_EQUAL_STRING(before.c_str(),state().c_str());
    std::string h=text(http(fill(raw)));std::string sh=state();
    TEST_ASSERT_EQUAL_STRING_MESSAGE(u.c_str(),h.c_str(),raw.c_str());TEST_ASSERT_EQUAL_STRING_MESSAGE(su.c_str(),sh.c_str(),raw.c_str());
}
void http_and_usb_give_identical_result_and_effect(){
    const char* cases[]={
        "{\"v\":1,\"id\":\"a\",\"cmd\":\"info\"}","{\"v\":1,\"id\":\"a\",\"cmd\":\"status\"}","{\"v\":1,\"id\":\"a\",\"cmd\":\"get_config\",\"view\":\"effective\",\"client\":\"x\"}",
        "{\"v\":1,\"id\":\"a\",\"cmd\":\"preview\",\"bootId\":\"$B\",\"client\":\"x\",\"revision\":$R,\"strip\":0,\"patch\":{\"brightness\":40}}",
        "{\"v\":1,\"id\":\"a\",\"cmd\":\"preview\",\"bootId\":\"$B\",\"client\":\"x\",\"revision\":$R,\"strip\":0,\"patch\":{\"brightness\":101}}",
        "{\"v\":1,\"id\":\"a\",\"cmd\":\"save\",\"bootId\":\"$B\",\"client\":\"x\",\"revision\":$R}",
        "{\"v\":1,\"id\":\"a\",\"cmd\":\"discard\",\"bootId\":\"$B\",\"client\":\"x\",\"revision\":$R}",
        "{\"v\":1,\"id\":\"a\",\"cmd\":\"touch\",\"bootId\":\"$B\",\"client\":\"x\",\"revision\":$R}",
        "{\"v\":1,\"id\":\"a\",\"cmd\":\"action\",\"bootId\":\"$B\",\"action\":\"master\",\"on\":true}",
        "{\"v\":1,\"id\":\"a\",\"cmd\":\"action\",\"bootId\":\"$B\",\"action\":\"reset\",\"channel\":-1}",
        "{\"v\":1,\"id\":\"a\",\"cmd\":\"action\",\"bootId\":\"$B\",\"action\":\"preset_save\",\"slot\":1,\"client\":\"x\",\"revision\":$R}",
        "{\"v\":1,\"id\":\"a\",\"cmd\":\"action\",\"bootId\":\"$B\",\"action\":\"preset_load\",\"slot\":2,\"client\":\"x\",\"revision\":$R}",
        "{\"v\":1,\"id\":\"a\",\"cmd\":\"action\",\"bootId\":\"$B\",\"action\":\"calibrate\",\"client\":\"x\",\"revision\":$R}",
        "{\"v\":1,\"id\":\"a\",\"cmd\":\"action\",\"bootId\":\"$B\",\"action\":\"reconnect\"}",
        "{\"v\":1,\"id\":\"a\",\"cmd\":\"action\",\"bootId\":\"$B\",\"action\":\"reboot\"}",
        "{\"v\":1,\"id\":\"a\",\"cmd\":\"nope\"}","{\"v\":2,\"id\":\"a\",\"cmd\":\"info\"}"};
    for(auto c:cases){setUp();parity(c);}
}
void each_command_success_and_failure(){
    expectOk(usb(previewMsg(255)));TEST_ASSERT_EQUAL_STRING("app",reply["result"]["previewOwner"]);TEST_ASSERT_EQUAL(60000,reply["result"]["previewRemainingMs"].as<int>());
    expectOk(usb(fill("{\"v\":1,\"id\":\"t\",\"cmd\":\"touch\",\"bootId\":\"$B\",\"client\":\"app\",\"revision\":$R}")));
    auto r=usb(fill("{\"v\":1,\"id\":\"s\",\"cmd\":\"save\",\"bootId\":\"$B\",\"client\":\"app\",\"revision\":$R}"));expectOk(r);
    TEST_ASSERT_EQUAL(r["revision"].as<uint32_t>(),r["savedRevision"].as<uint32_t>());TEST_ASSERT_FALSE(r["result"]["previewActive"]);TEST_ASSERT_EQUAL(255,stripSettings[1].color);
    expectError(usb(fill("{\"v\":1,\"id\":\"s\",\"cmd\":\"save\",\"bootId\":\"$B\",\"client\":\"app\",\"revision\":$R}")),"PREVIEW_EXPIRED");
    expectError(usb(fill("{\"v\":1,\"id\":\"t\",\"cmd\":\"touch\",\"bootId\":\"$B\",\"client\":\"app\",\"revision\":$R}")),"PREVIEW_EXPIRED");
    // apply = full config preview
    std::string cfg=settingsJson().c_str();size_t at=cfg.find("\"brightness\":100");TEST_ASSERT_TRUE(at!=std::string::npos);
    expectOk(usb(fill("{\"v\":1,\"id\":\"ap\",\"cmd\":\"apply\",\"bootId\":\"$B\",\"client\":\"app\",\"revision\":$R,\"config\":"+cfg+"}")));TEST_ASSERT_TRUE(settingsPreviewActive());
    expectOk(usb(fill("{\"v\":1,\"id\":\"d\",\"cmd\":\"discard\",\"bootId\":\"$B\",\"client\":\"app\",\"revision\":$R}")));TEST_ASSERT_FALSE(settingsPreviewActive());
    DynamicJsonDocument bad(24576);deserializeJson(bad,cfg);bad["strips"][0]["count"]=5000;std::string bs;serializeJson(bad,bs);
    expectError(usb(fill("{\"v\":1,\"id\":\"ap\",\"cmd\":\"apply\",\"bootId\":\"$B\",\"client\":\"app\",\"revision\":$R,\"config\":"+bs+"}")),"INVALID_ARGUMENT");TEST_ASSERT_FALSE(settingsPreviewActive());
    // actions
    expectOk(usb(fill("{\"v\":1,\"id\":\"m\",\"cmd\":\"action\",\"bootId\":\"$B\",\"action\":\"master\",\"on\":true}")));TEST_ASSERT_TRUE(drive.masterOn);TEST_ASSERT_TRUE(reply["result"]["master"]);
    uint32_t rev=settingsRevision();TEST_ASSERT_EQUAL(rev,reply["revision"].as<uint32_t>());
    expectError(usb(fill("{\"v\":1,\"id\":\"x\",\"cmd\":\"action\",\"bootId\":\"$B\",\"action\":\"reset\",\"channel\":3}")),"MASTER_MUST_BE_OFF");TEST_ASSERT_EQUAL(0,resets);
    expectError(usb(fill("{\"v\":1,\"id\":\"x\",\"cmd\":\"action\",\"bootId\":\"$B\",\"action\":\"reboot\"}")),"MASTER_MUST_BE_OFF");TEST_ASSERT_EQUAL(0,reboots);TEST_ASSERT_TRUE(drive.masterOn);
    expectOk(usb(fill("{\"v\":1,\"id\":\"m\",\"cmd\":\"action\",\"bootId\":\"$B\",\"action\":\"master\",\"on\":true}")));TEST_ASSERT_TRUE(drive.masterOn); // set, not toggle
    expectOk(usb(fill("{\"v\":1,\"id\":\"m\",\"cmd\":\"action\",\"bootId\":\"$B\",\"action\":\"master\",\"on\":false}")));TEST_ASSERT_FALSE(drive.masterOn);TEST_ASSERT_EQUAL(rev,settingsRevision());
    ready=false;expectError(usb(fill("{\"v\":1,\"id\":\"m\",\"cmd\":\"action\",\"bootId\":\"$B\",\"action\":\"master\",\"on\":true}")),"OUTPUT_BLOCKED");TEST_ASSERT_FALSE(drive.masterOn);ready=true;
    expectOk(usb(fill("{\"v\":1,\"id\":\"x\",\"cmd\":\"action\",\"bootId\":\"$B\",\"action\":\"reset\",\"channel\":3}")));TEST_ASSERT_EQUAL(3,lastReset);
    expectError(usb(fill("{\"v\":1,\"id\":\"x\",\"cmd\":\"action\",\"bootId\":\"$B\",\"action\":\"preset_load\",\"slot\":2,\"client\":\"app\",\"revision\":$R}")),"INVALID_ARGUMENT");
    expectOk(usb(fill("{\"v\":1,\"id\":\"x\",\"cmd\":\"action\",\"bootId\":\"$B\",\"action\":\"preset_save\",\"slot\":2,\"client\":\"app\",\"revision\":$R}")));TEST_ASSERT_TRUE(savedData.presets[2].used);
    expectOk(usb(fill("{\"v\":1,\"id\":\"x\",\"cmd\":\"action\",\"bootId\":\"$B\",\"action\":\"preset_load\",\"slot\":2,\"client\":\"app\",\"revision\":$R}")));
    imuGood=false;expectError(usb(fill("{\"v\":1,\"id\":\"x\",\"cmd\":\"action\",\"bootId\":\"$B\",\"action\":\"calibrate\",\"client\":\"app\",\"revision\":$R}")),"BUSY");TEST_ASSERT_EQUAL(0,calibrations);imuGood=true;
    expectOk(usb(fill("{\"v\":1,\"id\":\"x\",\"cmd\":\"action\",\"bootId\":\"$B\",\"action\":\"calibrate\",\"client\":\"app\",\"revision\":$R}")));TEST_ASSERT_EQUAL(1,calibrations);TEST_ASSERT_EQUAL_FLOAT(.5f,savedData.drive.accOff[0]);
    expectOk(usb(fill("{\"v\":1,\"id\":\"x\",\"cmd\":\"action\",\"bootId\":\"$B\",\"action\":\"reconnect\"}")));TEST_ASSERT_EQUAL(1,reconnects);
    expectOk(usb(fill("{\"v\":1,\"id\":\"x\",\"cmd\":\"action\",\"bootId\":\"$B\",\"action\":\"reboot\"}")));TEST_ASSERT_EQUAL(1,reboots);
    expectError(usb(fill("{\"v\":1,\"id\":\"x\",\"cmd\":\"action\",\"bootId\":\"$B\",\"action\":\"selfdestruct\"}")),"UNKNOWN_COMMAND");
    expectError(usb("{\"v\":1,\"id\":\"x\",\"cmd\":\"set_config\"}"),"UNKNOWN_COMMAND"); // legacy-only command is not v1
}
// 2. version, types, identifiers, unknown fields, malformed input
void envelope_validation(){
    std::string base=state();
    const char* badVersion[]={"{\"v\":2,\"id\":\"a\",\"cmd\":\"info\"}","{\"v\":\"1\",\"id\":\"a\",\"cmd\":\"info\"}","{\"v\":1.0,\"id\":\"a\",\"cmd\":\"info\"}","{\"v\":true,\"id\":\"a\",\"cmd\":\"info\"}","{\"v\":null,\"id\":\"a\",\"cmd\":\"info\"}"};
    for(auto m:badVersion){auto r=usb(m);expectError(r,"UNSUPPORTED_VERSION");}
    auto r=http("{\"id\":\"a\",\"cmd\":\"status\"}");expectError(r,"UNSUPPORTED_VERSION");TEST_ASSERT_EQUAL(400,lastHttp); // HTTP v1 route has no legacy path
    r=usb("[1,2]");TEST_ASSERT_TRUE(r.isNull()==false);
    r=http("[1,2]");expectError(r,"INVALID_ARGUMENT");TEST_ASSERT_TRUE(r["id"].isNull());
    r=http("\"str\"");expectError(r,"INVALID_ARGUMENT");
    const char* badIds[]={"{\"v\":1,\"cmd\":\"info\"}","{\"v\":1,\"id\":\"\",\"cmd\":\"info\"}","{\"v\":1,\"id\":5,\"cmd\":\"info\"}","{\"v\":1,\"id\":\"a\\u0000b\",\"cmd\":\"info\"}",
        "{\"v\":1,\"id\":\"a\\nb\",\"cmd\":\"info\"}","{\"v\":1,\"id\":\"a\\u007fb\",\"cmd\":\"info\"}","{\"v\":1,\"id\":\"\xd0\xb0\",\"cmd\":\"info\"}",
        "{\"v\":1,\"id\":\"0123456789012345678901234567890123456789012345678\",\"cmd\":\"info\"}"};
    for(auto m:badIds){r=usb(m);expectError(r,"INVALID_ARGUMENT");TEST_ASSERT_TRUE_MESSAGE(r["id"].isNull(),m);}
    expectOk(usb("{\"v\":1,\"id\":\"012345678901234567890123456789012345678901234567\",\"cmd\":\"info\"}")); // 48 bytes
    expectError(usb("{\"v\":1,\"id\":\"a\",\"cmd\":7}"),"INVALID_ARGUMENT");
    const char* badMut[]={
        "{\"v\":1,\"id\":\"a\",\"cmd\":\"action\",\"bootId\":\"$B\",\"action\":\"master\",\"on\":\"true\"}",
        "{\"v\":1,\"id\":\"a\",\"cmd\":\"action\",\"bootId\":\"$B\",\"action\":\"master\",\"on\":1}",
        "{\"v\":1,\"id\":\"a\",\"cmd\":\"action\",\"bootId\":\"$B\",\"action\":\"master\",\"on\":true,\"extra\":1}",
        "{\"v\":1,\"id\":\"a\",\"cmd\":\"action\",\"bootId\":\"$B\",\"action\":\"reset\",\"channel\":8}",
        "{\"v\":1,\"id\":\"a\",\"cmd\":\"action\",\"bootId\":\"$B\",\"action\":\"reset\",\"channel\":\"1\"}",
        "{\"v\":1,\"id\":\"a\",\"cmd\":\"action\",\"bootId\":\"$B\",\"action\":\"reset\",\"channel\":1.5}",
        "{\"v\":1,\"id\":\"a\",\"cmd\":\"action\",\"bootId\":\"$B\",\"action\":\"preset_save\",\"slot\":3,\"client\":\"x\",\"revision\":$R}",
        "{\"v\":1,\"id\":\"a\",\"cmd\":\"action\",\"bootId\":\"$B\",\"action\":\"preset_save\",\"slot\":1}",
        "{\"v\":1,\"id\":\"a\",\"cmd\":\"action\",\"bootId\":\"$B\",\"action\":\"reboot\",\"force\":true}",
        "{\"v\":1,\"id\":\"a\",\"cmd\":\"preview\",\"bootId\":\"$B\",\"client\":\"x\",\"revision\":\"$R\",\"strip\":0,\"patch\":{\"color\":1}}",
        "{\"v\":1,\"id\":\"a\",\"cmd\":\"preview\",\"bootId\":\"$B\",\"client\":\"x\",\"revision\":-1,\"strip\":0,\"patch\":{\"color\":1}}",
        "{\"v\":1,\"id\":\"a\",\"cmd\":\"preview\",\"bootId\":\"$B\",\"client\":\"x\",\"revision\":$R,\"strip\":8,\"patch\":{\"color\":1}}",
        "{\"v\":1,\"id\":\"a\",\"cmd\":\"preview\",\"bootId\":\"$B\",\"client\":\"x\",\"revision\":$R,\"strip\":0,\"patch\":{}}",
        "{\"v\":1,\"id\":\"a\",\"cmd\":\"preview\",\"bootId\":\"$B\",\"client\":\"x\",\"revision\":$R,\"strip\":0,\"patch\":{\"color\":\"255\"}}",
        "{\"v\":1,\"id\":\"a\",\"cmd\":\"preview\",\"bootId\":\"$B\",\"client\":\"x\",\"revision\":$R,\"strip\":0,\"patch\":{\"color\":255.0}}",
        "{\"v\":1,\"id\":\"a\",\"cmd\":\"preview\",\"bootId\":\"$B\",\"client\":\"x\",\"revision\":$R,\"strip\":0,\"patch\":{\"count\":10}}",
        "{\"v\":1,\"id\":\"a\",\"cmd\":\"preview\",\"bootId\":\"$B\",\"client\":\"x\",\"revision\":$R,\"strip\":0,\"patch\":{\"color\":1},\"x\":0}",
        "{\"v\":1,\"id\":\"a\",\"cmd\":\"preview\",\"bootId\":\"$B\",\"client\":\"a\\tb\",\"revision\":$R,\"strip\":0,\"patch\":{\"color\":1}}",
        "{\"v\":1,\"id\":\"a\",\"cmd\":\"preview\",\"bootId\":\"$B\",\"client\":\"\",\"revision\":$R,\"strip\":0,\"patch\":{\"color\":1}}",
        "{\"v\":1,\"id\":\"a\",\"cmd\":\"save\",\"bootId\":\"$B\",\"client\":\"x\"}",
        "{\"v\":1,\"id\":\"a\",\"cmd\":\"touch\",\"bootId\":7,\"client\":\"x\",\"revision\":$R}",
        "{\"v\":1,\"id\":\"a\",\"cmd\":\"touch\",\"client\":\"x\",\"revision\":$R}",
        "{\"v\":1,\"id\":\"a\",\"cmd\":\"apply\",\"bootId\":\"$B\",\"client\":\"x\",\"revision\":$R,\"config\":[1]}"};
    for(auto m:badMut){r=usb(fill(m));expectError(r,"INVALID_ARGUMENT");TEST_ASSERT_EQUAL_STRING_MESSAGE("a",r["id"]|"",m);}
    TEST_ASSERT_EQUAL_STRING(base.c_str(),state().c_str());TEST_ASSERT_EQUAL(0,masterPushes+resets+reboots+reconnects+calibrations);
    const char* broken[]={"{\"v\":1,\"id\":\"a\"","{\"v\":1,\"id\":\"a\",\"cmd\":\"info\"}x","{\"v\":1,\"id\":\"a\",\"cmd\":\"info\"}{}","","   ","{\"v\":1,\"id\":\"a\",\"cmd\":\"info\"","\x01","{v:1,id:\"a\",cmd:\"info\"}","{'v':1,'id':'a','cmd':'info'}","{\"v\":1,\"id\":\"a\",\"cmd\":\"info\",}"};
    for(auto m:broken){r=http(m);TEST_ASSERT_EQUAL_STRING_MESSAGE("BAD_JSON",r["error"]["code"]|"",m);TEST_ASSERT_TRUE(r["id"].isNull());TEST_ASSERT_EQUAL(400,lastHttp);}
    expectOk(http("{\"v\":1,\"id\":\"a\",\"cmd\":\"info\"}\r\n "));
    r=usb("{\"v\":1,\"id\":\"a\",\"cmd\":\"info\",\"pad\":1}");expectOk(r); // reads tolerate extra fields
}
// 3. editors, boot, revisions, lease
void boot_revision_and_editor_conflicts(){
    std::string base=state();
    expectError(usb(fill("{\"v\":1,\"id\":\"a\",\"cmd\":\"action\",\"bootId\":\"ffffffffffffffffffffffffffffffff\",\"action\":\"master\",\"on\":true}")),"BOOT_CHANGED");
    std::string msg=previewMsg(255);boot="aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"; // device rebooted: revision equal, bootId not
    auto r=usb(msg);expectError(r,"BOOT_CHANGED");TEST_ASSERT_EQUAL(409,httpStatus(Code::BootChanged));TEST_ASSERT_EQUAL_STRING(boot.c_str(),r["bootId"]);
    TEST_ASSERT_EQUAL_STRING(base.c_str(),state().c_str());
    expectError(usb(fill("{\"v\":1,\"id\":\"a\",\"cmd\":\"preview\",\"bootId\":\"$B\",\"client\":\"app\",\"revision\":999,\"strip\":1,\"patch\":{\"color\":1}}")),"REVISION_CONFLICT");
    expectOk(usb(previewMsg(255,"usb-editor")));std::string during=state();
    r=http(previewMsg(1,"http-editor"));expectError(r,"EDITOR_BUSY");TEST_ASSERT_EQUAL(409,lastHttp);
    expectError(http(fill("{\"v\":1,\"id\":\"a\",\"cmd\":\"save\",\"bootId\":\"$B\",\"client\":\"http-editor\",\"revision\":$R}")),"EDITOR_BUSY");
    expectError(http(fill("{\"v\":1,\"id\":\"a\",\"cmd\":\"discard\",\"bootId\":\"$B\",\"client\":\"http-editor\",\"revision\":$R}")),"EDITOR_BUSY");
    expectError(http("{\"v\":1,\"id\":\"a\",\"cmd\":\"get_config\",\"view\":\"effective\",\"client\":\"http-editor\"}"),"EDITOR_BUSY");
    expectError(http("{\"v\":1,\"id\":\"a\",\"cmd\":\"get_config\",\"view\":\"effective\"}"),"INVALID_ARGUMENT");
    expectError(http("{\"v\":1,\"id\":\"a\",\"cmd\":\"get_config\",\"view\":\"disk\"}"),"INVALID_ARGUMENT");
    r=usb("{\"v\":1,\"id\":\"a\",\"cmd\":\"get_config\",\"view\":\"effective\",\"client\":\"usb-editor\"}");expectOk(r);TEST_ASSERT_EQUAL(255,r["result"]["config"]["strips"][1]["color"].as<int>());
    r=usb("{\"v\":1,\"id\":\"a\",\"cmd\":\"get_config\"}");TEST_ASSERT_TRUE(r["result"]["config"]["strips"][1]["color"].as<int>()!=255);
    expectError(usb(fill("{\"v\":1,\"id\":\"a\",\"cmd\":\"action\",\"bootId\":\"$B\",\"action\":\"preset_save\",\"slot\":0,\"client\":\"usb-editor\",\"revision\":$R}")),"EDITOR_BUSY");
    expectError(usb(fill("{\"v\":1,\"id\":\"a\",\"cmd\":\"action\",\"bootId\":\"$B\",\"action\":\"calibrate\",\"client\":\"usb-editor\",\"revision\":$R}")),"EDITOR_BUSY");
    expectError(usb(fill("{\"v\":1,\"id\":\"a\",\"cmd\":\"action\",\"bootId\":\"$B\",\"action\":\"reconnect\"}")),"EDITOR_BUSY");
    TEST_ASSERT_EQUAL_STRING(during.c_str(),state().c_str());TEST_ASSERT_EQUAL(0,calibrations+reconnects);
    // lease expiry: the owner learns PREVIEW_EXPIRED, others see a revision conflict
    std::string stale=fill("{\"v\":1,\"id\":\"a\",\"cmd\":\"save\",\"bootId\":\"$B\",\"client\":\"usb-editor\",\"revision\":$R}");
    std::string stranger=fill("{\"v\":1,\"id\":\"a\",\"cmd\":\"preview\",\"bootId\":\"$B\",\"client\":\"other\",\"revision\":$R,\"strip\":1,\"patch\":{\"color\":2}}");
    std::string ownerPreview=fill("{\"v\":1,\"id\":\"a\",\"cmd\":\"preview\",\"bootId\":\"$B\",\"client\":\"usb-editor\",\"revision\":$R,\"strip\":1,\"patch\":{\"color\":2}}");
    testMillis=60000;settingsTick();TEST_ASSERT_FALSE(settingsPreviewActive());
    expectError(usb(stale),"PREVIEW_EXPIRED");expectError(usb(ownerPreview),"PREVIEW_EXPIRED");expectError(usb(stranger),"REVISION_CONFLICT");
    expectError(usb(fill("{\"v\":1,\"id\":\"a\",\"cmd\":\"action\",\"bootId\":\"$B\",\"action\":\"preset_save\",\"slot\":0,\"client\":\"x\",\"revision\":1}")),"REVISION_CONFLICT");
}
// 4. OTA: reads stay, every mutation path refuses the same way
void ota_blocks_every_mutation_on_every_path(){
    drive.masterOn=false;ota=true;std::string base=state();
    expectOk(usb("{\"v\":1,\"id\":\"a\",\"cmd\":\"status\"}"));expectOk(http("{\"v\":1,\"id\":\"a\",\"cmd\":\"info\"}"));expectOk(usb("{\"v\":1,\"id\":\"a\",\"cmd\":\"get_config\"}"));
    const char* v1[]={
        "{\"v\":1,\"id\":\"a\",\"cmd\":\"action\",\"bootId\":\"$B\",\"action\":\"master\",\"on\":true}",
        "{\"v\":1,\"id\":\"a\",\"cmd\":\"action\",\"bootId\":\"$B\",\"action\":\"master\",\"on\":false}",
        "{\"v\":1,\"id\":\"a\",\"cmd\":\"action\",\"bootId\":\"$B\",\"action\":\"reset\",\"channel\":-1}",
        "{\"v\":1,\"id\":\"a\",\"cmd\":\"action\",\"bootId\":\"$B\",\"action\":\"reboot\"}",
        "{\"v\":1,\"id\":\"a\",\"cmd\":\"action\",\"bootId\":\"$B\",\"action\":\"reconnect\"}",
        "{\"v\":1,\"id\":\"a\",\"cmd\":\"action\",\"bootId\":\"$B\",\"action\":\"preset_save\",\"slot\":0,\"client\":\"x\",\"revision\":$R}",
        "{\"v\":1,\"id\":\"a\",\"cmd\":\"action\",\"bootId\":\"$B\",\"action\":\"calibrate\",\"client\":\"x\",\"revision\":$R}",
        "{\"v\":1,\"id\":\"a\",\"cmd\":\"preview\",\"bootId\":\"$B\",\"client\":\"x\",\"revision\":$R,\"strip\":0,\"patch\":{\"color\":1}}"};
    for(auto m:v1){expectError(usb(fill(m)),"BUSY");expectError(http(fill(m)),"BUSY");TEST_ASSERT_EQUAL(409,lastHttp);}
    std::string cfg=settingsJson().c_str();
    const std::string legacy[]={"{\"cmd\":\"set_config\",\"config\":"+cfg+"}","{\"cmd\":\"action\",\"action\":\"master\",\"on\":true}","{\"cmd\":\"action\",\"action\":\"reboot\"}",
        "{\"cmd\":\"action\",\"action\":\"reset\",\"channel\":-1}","{\"cmd\":\"action\",\"action\":\"preset_save\",\"slot\":0}","{\"cmd\":\"action\",\"action\":\"calibrate\"}",
        "{\"cmd\":\"action\",\"action\":\"reconnect\"}",fill("{\"cmd\":\"preview\",\"client\":\"x\",\"revision\":$R,\"strip\":0,\"patch\":{\"color\":1}}")};
    for(auto& m:legacy){auto r=usb(m);TEST_ASSERT_FALSE_MESSAGE(r["ok"].as<bool>(),m.c_str());}
    int code;String out=legacyHttp("/api/config",cfg.data(),cfg.size(),code);TEST_ASSERT_EQUAL(400,code);
    std::string a="{\"action\":\"master\",\"on\":true}";out=legacyHttp("/api/action",a.data(),a.size(),code);TEST_ASSERT_EQUAL(400,code);
    std::string p=fill("{\"client\":\"x\",\"revision\":$R,\"strip\":0,\"patch\":{\"color\":1}}");out=legacyHttp("/api/preview",p.data(),p.size(),code);TEST_ASSERT_EQUAL(409,code);
    TEST_ASSERT_EQUAL_STRING(base.c_str(),state().c_str());TEST_ASSERT_EQUAL(0,masterPushes+resets+reboots+reconnects+calibrations);TEST_ASSERT_FALSE(drive.masterOn);
}
// 5. sizes and framing
static std::string padded(const std::string& head,size_t total){std::string s=head+",\"pad\":\"";std::string tail="\"}";s+=std::string(total-s.size()-tail.size(),'x');return s+tail;}
void size_limits(){
    std::string ok=padded("{\"v\":1,\"id\":\"a\",\"cmd\":\"status\"",MAX_COMMAND_BYTES);TEST_ASSERT_EQUAL(24576,ok.size());
    expectOk(usb(ok));expectOk(http(ok));
    std::string big=padded("{\"v\":1,\"id\":\"a\",\"cmd\":\"status\"",MAX_COMMAND_BYTES+1);
    auto r=usb(big);expectError(r,"FRAME_TOO_LARGE");TEST_ASSERT_TRUE(r["id"].isNull());r=http(big);expectError(r,"FRAME_TOO_LARGE");TEST_ASSERT_EQUAL(413,lastHttp);
    std::string legacyOk=padded("{\"cmd\":\"status\"",MAX_LEGACY_BYTES),legacyBig=padded("{\"cmd\":\"status\"",MAX_LEGACY_BYTES+1);
    r=usb(legacyOk);TEST_ASSERT_TRUE(r.containsKey("configRevision"));r=usb(legacyBig);TEST_ASSERT_EQUAL_STRING("line too long",r["error"]|"");
    // nested config measured as minified JSON
    DynamicJsonDocument d(65536);deserializeJson(d,settingsJson().c_str());d["pad"]="";
    size_t baseSize=measureJson(d);d["pad"]=std::string(MAX_CONFIG_BYTES-baseSize,'y');TEST_ASSERT_EQUAL(MAX_CONFIG_BYTES,measureJson(d));
    std::string cfg;serializeJson(d,cfg);
    expectOk(usb(fill("{\"v\":1,\"id\":\"a\",\"cmd\":\"apply\",\"bootId\":\"$B\",\"client\":\"x\",\"revision\":$R,\"config\":"+cfg+"}")));
    expectOk(usb(fill("{\"v\":1,\"id\":\"a\",\"cmd\":\"discard\",\"bootId\":\"$B\",\"client\":\"x\",\"revision\":$R}")));
    d.clear();deserializeJson(d,settingsJson().c_str());d["pad"]=std::string(MAX_CONFIG_BYTES-baseSize+1,'y');cfg.clear();serializeJson(d,cfg);std::string before=state();TEST_ASSERT_EQUAL_MESSAGE(MAX_CONFIG_BYTES+1,measureJson(d),"measure");
    r=usb(fill("{\"v\":1,\"id\":\"a\",\"cmd\":\"apply\",\"bootId\":\"$B\",\"client\":\"x\",\"revision\":$R,\"config\":"+cfg+"}"));expectError(r,"FRAME_TOO_LARGE");TEST_ASSERT_EQUAL_STRING(before.c_str(),state().c_str());
    // deep nesting is refused, not crashed
    std::string deep="{\"v\":1,\"id\":\"a\",\"cmd\":\"status\",\"x\":"+std::string(40,'[')+std::string(40,']')+"}";expectError(usb(deep),"FRAME_TOO_LARGE");
    r=usb("{\"v\":1");expectError(r,"BAD_JSON");
}
// 6. flash failure: stable code, preview/lease/snapshot kept
void flash_failure_keeps_preview_and_lease(){
    expectOk(usb(previewMsg(255)));uint32_t saved;settingsSavedRevision(saved);testMillis=10000;
    LittleFS.failWrite=true;auto r=http(fill("{\"v\":1,\"id\":\"s\",\"cmd\":\"save\",\"bootId\":\"$B\",\"client\":\"app\",\"revision\":$R}"));
    expectError(r,"FLASH_WRITE_FAILED");TEST_ASSERT_EQUAL(507,lastHttp);TEST_ASSERT_TRUE(settingsPreviewActive());TEST_ASSERT_EQUAL_STRING("app",settingsPreviewOwner());
    TEST_ASSERT_EQUAL(saved,r["savedRevision"].as<uint32_t>());TEST_ASSERT_TRUE(r["revision"].as<uint32_t>()!=saved);TEST_ASSERT_EQUAL(50000,settingsPreviewRemainingMs());
    LittleFS.failWrite=false;expectOk(http(fill("{\"v\":1,\"id\":\"s\",\"cmd\":\"save\",\"bootId\":\"$B\",\"client\":\"app\",\"revision\":$R}")));
}
// 7. SSE v1 snapshot coherence; HTTP auth
void sse_v1_and_auth(){
    String s=sseV1(7);DynamicJsonDocument d(4096);TEST_ASSERT_FALSE(deserializeJson(d,s.c_str()));
    TEST_ASSERT_EQUAL(1,d["v"].as<int>());TEST_ASSERT_EQUAL(7,d["seq"].as<int>());TEST_ASSERT_EQUAL_STRING(boot.c_str(),d["bootId"]);
    TEST_ASSERT_EQUAL(d["revision"].as<uint32_t>(),d["status"]["configRevision"].as<uint32_t>());TEST_ASSERT_EQUAL(d["savedRevision"].as<uint32_t>(),d["status"]["savedRevision"].as<uint32_t>());
    TEST_ASSERT_EQUAL_STRING(boot.c_str(),d["status"]["bootId"]);
    String legacy=statusJson();TEST_ASSERT_FALSE(deserializeJson(d,legacy.c_str()));TEST_ASSERT_FALSE(d.containsKey("v"));TEST_ASSERT_TRUE(d.containsKey("configRevision"));
    auto r=http("{\"v\":1,\"id\":\"a\",\"cmd\":\"status\"}",false);expectError(r,"AUTH_REQUIRED");TEST_ASSERT_EQUAL(403,lastHttp);TEST_ASSERT_EQUAL_STRING("a",r["id"]);
    std::string before=state();expectError(http(previewMsg(1),false),"AUTH_REQUIRED");TEST_ASSERT_EQUAL_STRING(before.c_str(),state().c_str());
}
// 8. legacy payloads keep their shape
void legacy_payloads_unchanged(){
    auto r=usb("{\"cmd\":\"get_config\"}");TEST_ASSERT_EQUAL(5,r["schema"].as<int>());
    r=usb("{\"cmd\":\"action\",\"action\":\"master\",\"on\":true}");TEST_ASSERT_TRUE(r["ok"]);TEST_ASSERT_TRUE(r["master"]);TEST_ASSERT_TRUE(r.containsKey("revision"));TEST_ASSERT_TRUE(r.containsKey("savedRevision"));TEST_ASSERT_FALSE(r.containsKey("v"));
    r=usb("{\"cmd\":\"action\",\"action\":\"reboot\"}");TEST_ASSERT_TRUE(r["ok"]);TEST_ASSERT_FALSE(drive.masterOn);TEST_ASSERT_EQUAL(1,reboots); // legacy reboot still switches Master off itself
    r=usb("{\"cmd\":\"nope\"}");TEST_ASSERT_FALSE(r["ok"]);TEST_ASSERT_EQUAL_STRING("Unknown command",r["error"]);
    r=usb(fill("{\"cmd\":\"preview\",\"client\":\"web\",\"revision\":$R,\"strip\":0,\"patch\":{\"color\":9}}"));TEST_ASSERT_TRUE(r["ok"]);
    r=usb(fill("{\"cmd\":\"preview\",\"client\":\"usb\",\"revision\":$R,\"strip\":0,\"patch\":{\"color\":9}}"));TEST_ASSERT_FALSE(r["ok"]);TEST_ASSERT_EQUAL_STRING("Configuration changed or another editor is active; reload",r["error"]);
    int code;std::string p=fill("{\"client\":\"usb\",\"revision\":$R}");String out=legacyHttp("/api/save",p.data(),p.size(),code);TEST_ASSERT_EQUAL(409,code);
    p=fill("{\"client\":\"web\",\"revision\":$R}");out=legacyHttp("/api/save",p.data(),p.size(),code);TEST_ASSERT_EQUAL(200,code);
    DynamicJsonDocument d(512);deserializeJson(d,out.c_str());TEST_ASSERT_TRUE(d["ok"]);TEST_ASSERT_EQUAL(d["revision"].as<uint32_t>(),d["savedRevision"].as<uint32_t>());
    std::string cfg=settingsJson().c_str();out=legacyHttp("/api/config",cfg.data(),cfg.size(),code);TEST_ASSERT_EQUAL(200,code);TEST_ASSERT_EQUAL_STRING("{\"ok\":true,\"master\":false}",out.c_str());
    std::string bad="{nope";out=legacyHttp("/api/action",bad.data(),bad.size(),code);TEST_ASSERT_EQUAL(400,code);TEST_ASSERT_EQUAL_STRING("{\"error\":\"Invalid JSON\"}",out.c_str());
    std::string huge(24001,' ');out=legacyHttp("/api/config",huge.data(),huge.size(),code);TEST_ASSERT_EQUAL(413,code);
    r=usb("not json");expectError(r,"BAD_JSON");
}

// R2: U+0000 must not shorten names/keys or merge keys; duplicates are refused too
static int hookCalls(){return masterPushes+resets+reboots+reconnects+calibrations;}
void nul_and_duplicate_keys_refused_without_effect(){
    std::string cases[]={
        "{\"v\":1,\"id\":\"x\",\"cmd\":\"action\\u0000junk\",\"bootId\":\"$B\",\"action\":\"master\",\"on\":true}",
        "{\"v\":1,\"id\":\"x\",\"cmd\":\"action\",\"bootId\":\"$B\",\"action\":\"master\\u0000junk\",\"on\":true}",
        "{\"v\":1,\"id\":\"x\",\"cmd\":\"action\",\"bootId\":\"$B\\u0000junk\",\"action\":\"master\",\"on\":true}",
        "{\"v\":1,\"id\":\"x\",\"cmd\":\"action\",\"bootId\":\"$B\",\"action\":\"master\",\"on\":true,\"on\\u0000junk\":false}",
        "{\"v\":1,\"id\":\"x\",\"cmd\":\"preview\",\"bootId\":\"$B\",\"client\":\"app\",\"revision\":$R,\"strip\":0,\"patch\":{\"color\\u0000junk\":321}}",
        "{\"v\":1,\"id\":\"x\",\"cmd\":\"action\",\"bootId\":\"$B\",\"action\":\"master\",\"on\":false,\"on\":true}",
        "{\"v\":1,\"id\":\"x\",\"cmd\":\"preview\",\"bootId\":\"$B\",\"client\":\"app\",\"revision\":$R,\"strip\":0,\"patch\":{\"color\":1,\"\\u0063olor\":2}}",
        "{\"v\":1,\"id\":\"x\",\"cmd\":\"get_config\",\"view\":\"effective\\u0000\",\"client\":\"app\"}"};
    for(auto& raw:cases){
        std::string m=fill(raw),before=state();
        auto u=usb(m);std::string ut=text(u);expectError(u,"INVALID_ARGUMENT");TEST_ASSERT_TRUE_MESSAGE(u["id"].isNull(),m.c_str());
        TEST_ASSERT_EQUAL_STRING_MESSAGE(before.c_str(),state().c_str(),m.c_str());
        auto h=http(m);TEST_ASSERT_EQUAL_STRING_MESSAGE(ut.c_str(),text(h).c_str(),m.c_str());TEST_ASSERT_EQUAL(400,lastHttp);
        TEST_ASSERT_EQUAL_STRING_MESSAGE(before.c_str(),state().c_str(),m.c_str());TEST_ASSERT_FALSE(drive.masterOn);
    }
    TEST_ASSERT_EQUAL(0,hookCalls());
    // legacy paths use the same rule
    std::string before=state();
    std::string lp=fill("{\"client\":\"web\",\"revision\":$R,\"strip\":0,\"patch\":{\"color\\u0000junk\":321}}");int code;
    String out=legacyHttp("/api/preview",lp.data(),lp.size(),code);TEST_ASSERT_EQUAL(400,code);TEST_ASSERT_EQUAL_STRING("{\"error\":\"Invalid JSON\"}",out.c_str());
    std::string la="{\"action\":\"master\",\"on\":true,\"on\":false}";out=legacyHttp("/api/action",la.data(),la.size(),code);TEST_ASSERT_EQUAL(400,code);
    auto r=usb("{\"cmd\":\"action\",\"action\":\"master\\u0000x\",\"on\":true}");expectError(r,"INVALID_ARGUMENT");
    TEST_ASSERT_EQUAL_STRING(before.c_str(),state().c_str());TEST_ASSERT_EQUAL(0,hookCalls());
    // a literal backslash-u0000 (no NUL) is an ordinary string
    expectOk(usb("{\"v\":1,\"id\":\"a\\\\u0000\",\"cmd\":\"info\"}"));TEST_ASSERT_EQUAL_STRING("a\\u0000",reply["id"]);
}
// R3: USB keeps body bytes; a CR inside the body is the client's message, not noise
static std::vector<std::string> usbStream(const std::string& bytes){
    LineFramer f(MAX_COMMAND_BYTES);std::vector<std::string> out;
    for(char c:bytes){auto e=f.feed(c);if(e==LineFramer::Overflow)out.push_back(usbOverflow().c_str());if(e==LineFramer::Line){const std::string& l=f.text();out.push_back(handleUsbLine(l.data(),l.size()).c_str());}}
    return out;
}
void usb_cr_inside_body_is_not_removed(){
    std::string raw=fill("{\"v\":1,\"id\":\"x\",\"cmd\":\"action\",\"bootId\":\"$B\",\"action\":\"master\",\"on\":true}");
    std::string withCr=raw;withCr.insert(withCr.find("action")+3,1,'\r');
    auto out=usbStream(withCr+"\n");TEST_ASSERT_EQUAL(1,out.size());TEST_ASSERT_NOT_NULL(strstr(out[0].c_str(),"BAD_JSON"));TEST_ASSERT_FALSE(drive.masterOn);
    auto h=http(withCr);expectError(h,"BAD_JSON");TEST_ASSERT_EQUAL_STRING(out[0].c_str(),text(h).c_str());TEST_ASSERT_FALSE(drive.masterOn);
    out=usbStream(raw+std::string(MAX_COMMAND_BYTES,'\r')+"\n"+fill("{\"v\":1,\"id\":\"next\",\"cmd\":\"info\"}\r\n"));
    TEST_ASSERT_EQUAL(2,out.size());TEST_ASSERT_NOT_NULL(strstr(out[0].c_str(),"FRAME_TOO_LARGE"));TEST_ASSERT_NOT_NULL(strstr(out[1].c_str(),"\"id\":\"next\""));TEST_ASSERT_FALSE(drive.masterOn);
    out=usbStream(raw+"\r\n");TEST_ASSERT_EQUAL(1,out.size());TEST_ASSERT_NOT_NULL(strstr(out[0].c_str(),"\"ok\":true"));TEST_ASSERT_TRUE(drive.masterOn); // CRLF clients still work
    drive.masterOn=false;
    std::string at=padded("{\"v\":1,\"id\":\"a\",\"cmd\":\"status\"",MAX_COMMAND_BYTES);
    out=usbStream(at+"\r\n");TEST_ASSERT_NOT_NULL(strstr(out[0].c_str(),"\"ok\":true"));
    out=usbStream(at.substr(0,at.size()-2)+"x\"}\r\n");TEST_ASSERT_NOT_NULL(strstr(out[0].c_str(),"FRAME_TOO_LARGE"));
    out=usbStream("{\"cmd\":\"status\"}\r\n");TEST_ASSERT_NOT_NULL(strstr(out[0].c_str(),"configRevision")); // legacy CRLF
}
// Independent of lib/strict_json so the check also runs against older builds.
static bool utf8ok(const char* s){const unsigned char* p=(const unsigned char*)s;while(*p){int n=*p<0x80?1:(*p>>5)==6&&*p>=0xC2?2:(*p>>4)==14?3:(*p>>3)==30&&*p<=0xF4?4:0;if(!n)return false;
    for(int i=1;i<n;++i)if((p[i]&0xC0)!=0x80)return false;if(n==3&&((*p==0xE0&&p[1]<0xA0)||(*p==0xED&&p[1]>0x9F)))return false;if(n==4&&((*p==0xF0&&p[1]<0x90)||(*p==0xF4&&p[1]>0x8F)))return false;p+=n;}return true;}
// R4: only valid UTF-8 reaches settings; valid Unicode round-trips byte for byte
static std::string configWithSsid(const std::string& ssidJson){
    std::string cfg=settingsJson().c_str();size_t a=cfg.find("\"ssid\":\"");size_t b=cfg.find('"',a+8);return cfg.substr(0,a+8)+ssidJson+cfg.substr(b);
}
void invalid_utf8_refused_valid_unicode_round_trips(){
    const char* bad[]={"\xff","ab\x80","\xc3","\xe2\x82","\xc0\xaf","\xed\xa0\x80","\xf4\x90\x80\x80","\\ud800"};
    for(auto b:bad){
        std::string before=state();uint32_t writes=LittleFS.writes;
        std::string m=fill("{\"v\":1,\"id\":\"a\",\"cmd\":\"apply\",\"bootId\":\"$B\",\"client\":\"app\",\"revision\":$R,\"config\":"+configWithSsid(b)+"}");
        expectError(usb(m),"BAD_JSON");expectError(http(m),"BAD_JSON");
        std::string legacy="{\"cmd\":\"set_config\",\"config\":"+configWithSsid(b)+"}";expectError(usb(legacy),"BAD_JSON");
        testMillis+=2000;settingsTick();TEST_ASSERT_EQUAL_STRING(before.c_str(),state().c_str());TEST_ASSERT_EQUAL(writes,LittleFS.writes);TEST_ASSERT_FALSE(settingsPreviewActive());
    }
    struct { const char* json; const char* bytes; } good[]={{"\xd0\x96\xd1\x83\xd0\xba \xf0\x9f\x98\x80","\xd0\x96\xd1\x83\xd0\xba \xf0\x9f\x98\x80"},{"\\u0416\\ud83d\\ude00 \xe2\x82\xac","\xd0\x96\xf0\x9f\x98\x80 \xe2\x82\xac"}};
    for(auto& g:good){
        expectOk(usb(fill("{\"v\":1,\"id\":\"a\",\"cmd\":\"apply\",\"bootId\":\"$B\",\"client\":\"app\",\"revision\":$R,\"config\":"+configWithSsid(g.json)+"}")));
        auto s=usb(fill("{\"v\":1,\"id\":\"s\",\"cmd\":\"save\",\"bootId\":\"$B\",\"client\":\"app\",\"revision\":$R}"));expectOk(s);TEST_ASSERT_EQUAL(s["revision"].as<uint32_t>(),s["savedRevision"].as<uint32_t>());
        std::string raw="{\"v\":1,\"id\":\"g\",\"cmd\":\"get_config\"}";String out=handleUsbLine(raw.data(),raw.size());
        TEST_ASSERT_TRUE(utf8ok(out.c_str()));
        DynamicJsonDocument d(32768);deserializeJson(d,out.c_str());TEST_ASSERT_EQUAL_STRING(g.bytes,d["result"]["config"]["network"]["ssid"]);
        TEST_ASSERT_NOT_NULL(strstr(LittleFS.files["/config-v3.json"]->c_str(),g.bytes));
    }
}
int main(){UNITY_BEGIN();RUN_TEST(nul_and_duplicate_keys_refused_without_effect);RUN_TEST(usb_cr_inside_body_is_not_removed);RUN_TEST(invalid_utf8_refused_valid_unicode_round_trips);RUN_TEST(envelope_shape_and_reads);RUN_TEST(http_and_usb_give_identical_result_and_effect);RUN_TEST(each_command_success_and_failure);RUN_TEST(envelope_validation);RUN_TEST(boot_revision_and_editor_conflicts);RUN_TEST(ota_blocks_every_mutation_on_every_path);RUN_TEST(size_limits);RUN_TEST(flash_failure_keeps_preview_and_lease);RUN_TEST(sse_v1_and_auth);RUN_TEST(legacy_payloads_unchanged);return UNITY_END();}
