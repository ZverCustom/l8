#include <unity.h>
#include <Arduino.h>
#include <LittleFS.h>
#include "../../src/settings/settings.cpp"
SerialStub Serial; uint32_t testMillis=0; FakeFS LittleFS;
CanSettings can_setting; StripSettings stripSettings[NUM_STRIPS]; int numLedsPerStrip[NUM_STRIPS];
int canMode=2,canBitrateSel=1; String wifiSsid,wifiPass;
const char* netDefaultSsid(){return "";}const char* netDefaultPass(){return "";}
void vehicleSetMode(int n){canMode=n;}void vehicleSetBitrate(int n){canBitrateSel=n;}
void ledsPause(){}void ledsResume(){}void ledsFlashPause(){}void ledsFlashResume(){}void requestLedReinit(){}void powerTick(){}
static DynamicJsonDocument request(1024);
static void identity(const char* client="one") {request.clear();request["client"]=client;request["revision"]=settingsRevision();}
static bool preview(int color,const char* client="one") {identity(client);request["strip"]=0;request["patch"]["color"]=color;String e;return settingsPreview(request.as<JsonVariantConst>(),false,e);}
static bool commit(){identity();String e;return settingsCommitPreview(request.as<JsonVariantConst>(),e);}
static int diskColor(){DynamicJsonDocument d(24576);deserializeJson(d,*LittleFS.files["/config-v3.json"]);return d["strips"][0]["color"];}
void setUp(){LittleFS.files.clear();LittleFS.failWrite=false;LittleFS.failInstall=false;LittleFS.writes=0;testMillis=0;settingsBegin();settingsSave();TEST_ASSERT_TRUE(settingsSaveNow());}
void tearDown(){}
void no_preview_leak_to_pending_or_backup(){
    int base=stripSettings[0].color;
    settingsSave();TEST_ASSERT_TRUE(preview(255));drive.masterOn=true;
    TEST_ASSERT_EQUAL(255,stripSettings[0].color);TEST_ASSERT_TRUE(drive.masterOn);
    testMillis=1600;settingsTick();TEST_ASSERT_EQUAL(base,diskColor());
    DynamicJsonDocument d(24576);deserializeJson(d,settingsJson(true));TEST_ASSERT_EQUAL(base,d["strips"][0]["color"].as<int>());
    deserializeJson(d,settingsJson(false,true));TEST_ASSERT_EQUAL(base,d["strips"][0]["color"].as<int>());
    TEST_ASSERT_TRUE(settingsSaveNow());TEST_ASSERT_EQUAL(base,diskColor()); // reboot flush
    TEST_ASSERT_TRUE(commit());TEST_ASSERT_EQUAL(255,diskColor());TEST_ASSERT_TRUE(drive.masterOn);
}
void revision_and_second_client(){
    TEST_ASSERT_TRUE(preview(255));uint32_t before=settingsRevision();
    TEST_ASSERT_FALSE(preview(65280,"two"));TEST_ASSERT_EQUAL(before,settingsRevision());TEST_ASSERT_EQUAL(255,stripSettings[0].color);
    identity();request["revision"]=before-1;String error;
    TEST_ASSERT_FALSE(settingsCommitPreview(request.as<JsonVariantConst>(),error));TEST_ASSERT_TRUE(settingsPreviewActive());
}
void expiry_and_manual_discard(){
    int base=stripSettings[0].color;TEST_ASSERT_TRUE(preview(255));drive.masterOn=true;
    testMillis=59999;settingsTick();TEST_ASSERT_EQUAL(255,stripSettings[0].color);
    testMillis=60000;settingsTick();TEST_ASSERT_EQUAL(base,stripSettings[0].color);TEST_ASSERT_TRUE(drive.masterOn);
    TEST_ASSERT_TRUE(preview(65280));identity();String error;TEST_ASSERT_TRUE(settingsDiscardPreview(request.as<JsonVariantConst>(),error));TEST_ASSERT_EQUAL(base,stripSettings[0].color);
}
void flash_failure_is_not_a_save(){
    int base=diskColor();settingsSave();TEST_ASSERT_TRUE(preview(255));uint32_t rev=settingsRevision();
    LittleFS.failWrite=true;TEST_ASSERT_FALSE(commit());TEST_ASSERT_EQUAL(rev,settingsRevision());TEST_ASSERT_TRUE(settingsPreviewActive());TEST_ASSERT_EQUAL(base,diskColor());
    LittleFS.failWrite=false;testMillis=1600;settingsTick();TEST_ASSERT_EQUAL(base,diskColor());
    TEST_ASSERT_TRUE(commit());TEST_ASSERT_EQUAL(255,diskColor());
}
void failed_install_recovers_backup_without_preview(){
    int base=diskColor();TEST_ASSERT_TRUE(preview(255));LittleFS.failInstall=true;
    TEST_ASSERT_FALSE(commit());TEST_ASSERT_FALSE(LittleFS.exists("/config-v3.json"));TEST_ASSERT_TRUE(LittleFS.exists("/config-v3.bak"));
    LittleFS.failInstall=false;settingsBegin();TEST_ASSERT_EQUAL(base,stripSettings[0].color);TEST_ASSERT_FALSE(settingsPreviewActive());
}
void invalid_patch_atomic(){
    int base=stripSettings[0].color;identity();request["strip"]=0;request["patch"]["color"]=255;request["patch"]["brightness"]=101;String error;
    TEST_ASSERT_FALSE(settingsPreview(request.as<JsonVariantConst>(),false,error));TEST_ASSERT_EQUAL(base,stripSettings[0].color);TEST_ASSERT_FALSE(settingsPreviewActive());
}
void legacy_speed_and_order_migration(){
    DynamicJsonDocument d(24576);deserializeJson(d,settingsJson());d["schema"]=3;d["global"]["speed"]=23;
    for(int i=0;i<8;++i){d["strips"][i]["color"]=i;d["strips"][i].remove("speed");}
    String error;TEST_ASSERT_TRUE_MESSAGE(settingsApply(d.as<JsonVariantConst>(),error),error.c_str());
    for(int i=0;i<8;++i){TEST_ASSERT_EQUAL(v3::hardwareChannel(i),stripSettings[i].color);TEST_ASSERT_EQUAL(23,stripSettings[i].speed);}
}
void lease_touch_wrap(){
    testMillis=0xfffffff0;TEST_ASSERT_TRUE(preview(255));testMillis+=59000;identity();String error;TEST_ASSERT_TRUE(settingsTouchPreview(request.as<JsonVariantConst>(),error));
    testMillis+=59000;settingsTick();TEST_ASSERT_TRUE(settingsPreviewActive());testMillis+=1000;settingsTick();TEST_ASSERT_FALSE(settingsPreviewActive());
}
int main(){UNITY_BEGIN();RUN_TEST(no_preview_leak_to_pending_or_backup);RUN_TEST(revision_and_second_client);RUN_TEST(expiry_and_manual_discard);RUN_TEST(flash_failure_is_not_a_save);RUN_TEST(invalid_patch_atomic);RUN_TEST(failed_install_recovers_backup_without_preview);RUN_TEST(legacy_speed_and_order_migration);RUN_TEST(lease_touch_wrap);return UNITY_END();}
