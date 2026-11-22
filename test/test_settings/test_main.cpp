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
void setUp(){LittleFS.files.clear();LittleFS.failWrite=false;LittleFS.failInstall=false;LittleFS.failBackup=false;LittleFS.failBegin=false;LittleFS.shortWrite=false;LittleFS.writes=0;testMillis=0;settingsBegin();settingsSave();TEST_ASSERT_TRUE(settingsSaveNow());}
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

// ---- savedRevision matrix (task 002) ----
static const char* MAIN="/config-v3.json"; static const char* BAK="/config-v3.bak";
static int64_t saved(){uint32_t v;return settingsSavedRevision(v)?int64_t(v):-1;}
static bool isSaved(){return saved()==int64_t(settingsRevision());}
static int fileColor(const char* path){DynamicJsonDocument d(24576);if(deserializeJson(d,*LittleFS.files[path]))return -1;return d["strips"][0]["color"];}
static void edit(int color){DynamicJsonDocument d(24576);deserializeJson(d,settingsJson(true));d["strips"][0]["color"]=color;d["network"]["keepPassword"]=true;String e;TEST_ASSERT_TRUE_MESSAGE(settingsApply(d.as<JsonVariantConst>(),e),e.c_str());settingsSave();}
static void flushAt(uint32_t t){testMillis=t;settingsTick();}
static bool discard(){identity();String e;return settingsDiscardPreview(request.as<JsonVariantConst>(),e);}
static void reboot(){LittleFS.failWrite=LittleFS.failInstall=LittleFS.failBackup=LittleFS.shortWrite=false;testMillis=0;settingsBegin();}
void m1_defaults_without_file_are_not_saved_and_not_written(){
    LittleFS.files.clear();LittleFS.writes=0;settingsBegin();TEST_ASSERT_EQUAL(-1,saved());
    flushAt(5000);TEST_ASSERT_EQUAL(0,LittleFS.writes);TEST_ASSERT_FALSE(LittleFS.exists(MAIN));TEST_ASSERT_EQUAL(-1,saved());
}
void m1_schema5_main_and_backup_load_as_saved(){
    settingsBegin();TEST_ASSERT_TRUE(isSaved());
    LittleFS.files[BAK]=LittleFS.files[MAIN];LittleFS.files.erase(MAIN);settingsBegin();TEST_ASSERT_TRUE(isSaved());
    LittleFS.files[MAIN]=std::make_shared<std::string>("{broken");settingsBegin();TEST_ASSERT_TRUE(isSaved());
}
void m1_unavailable_fs_is_never_saved(){
    LittleFS.failBegin=true;settingsBegin();TEST_ASSERT_EQUAL(-1,saved());edit(255);TEST_ASSERT_FALSE(settingsSaveNow());TEST_ASSERT_EQUAL(-1,saved());
}
void m1_migration_is_saved_only_after_its_write(){
    DynamicJsonDocument d(24576);deserializeJson(d,*LittleFS.files[MAIN]);d["schema"]=4;d["strips"][0]["color"]=77;for(int i=0;i<8;++i)d["strips"][i].remove("speed");
    for(auto p:d["presets"].as<JsonArray>())if(p["used"])for(auto st:p["strips"].as<JsonArray>())st.remove("speed");d["global"].remove("autoRetry");
    std::string old;serializeJson(d,old);LittleFS.files[MAIN]=std::make_shared<std::string>(old);
    LittleFS.writes=0;settingsBegin();TEST_ASSERT_EQUAL(77,stripSettings[0].color);TEST_ASSERT_EQUAL(-1,saved());TEST_ASSERT_EQUAL(0,LittleFS.writes);
    flushAt(1600);TEST_ASSERT_EQUAL(1,LittleFS.writes);TEST_ASSERT_TRUE(isSaved());
    deserializeJson(d,*LittleFS.files[MAIN]);TEST_ASSERT_EQUAL(5,d["schema"].as<int>());TEST_ASSERT_EQUAL(77,d["strips"][0]["color"].as<int>());
}
void m1_failed_migration_write_stays_unsaved(){
    DynamicJsonDocument d(24576);deserializeJson(d,*LittleFS.files[MAIN]);d["schema"]=4;for(int i=0;i<8;++i)d["strips"][i].remove("speed");d["global"].remove("autoRetry");
    std::string old;serializeJson(d,old);LittleFS.files[MAIN]=std::make_shared<std::string>(old);
    settingsBegin();LittleFS.failWrite=true;flushAt(1600);TEST_ASSERT_EQUAL(-1,saved());TEST_ASSERT_EQUAL_STRING(old.c_str(),LittleFS.files[MAIN]->c_str());
}
void m2_change_is_saved_only_after_the_delayed_write(){
    int64_t before=saved();uint32_t writes=LittleFS.writes;edit(255);
    TEST_ASSERT_EQUAL(before,saved());TEST_ASSERT_FALSE(isSaved());
    flushAt(1499);TEST_ASSERT_EQUAL(writes,LittleFS.writes);TEST_ASSERT_FALSE(isSaved());
    flushAt(1500);TEST_ASSERT_EQUAL(writes+1,LittleFS.writes);TEST_ASSERT_TRUE(isSaved());TEST_ASSERT_EQUAL(255,fileColor(MAIN));
}
void m3_old_pending_written_during_preview_keeps_current_dirty(){
    edit(255);uint32_t pendingRev=settingsRevision();TEST_ASSERT_TRUE(preview(65280));
    flushAt(1600);TEST_ASSERT_EQUAL(255,fileColor(MAIN));TEST_ASSERT_EQUAL(pendingRev,saved());TEST_ASSERT_FALSE(isSaved());
    TEST_ASSERT_TRUE(commit());TEST_ASSERT_TRUE(isSaved());TEST_ASSERT_EQUAL(65280,fileColor(MAIN));
}
void m4_commit_success_reports_consistent_revisions(){
    uint32_t writes=LittleFS.writes;TEST_ASSERT_TRUE(preview(255));TEST_ASSERT_FALSE(isSaved());
    TEST_ASSERT_TRUE(commit());TEST_ASSERT_TRUE(isSaved());TEST_ASSERT_FALSE(settingsPreviewActive());TEST_ASSERT_EQUAL(writes+1,LittleFS.writes);
    TEST_ASSERT_EQUAL(255,fileColor(MAIN));flushAt(5000);TEST_ASSERT_EQUAL(writes+1,LittleFS.writes);
}
static void failedCommitKeepsEverything(bool FakeFS::*flag,bool mainSurvives){
    int base=fileColor(MAIN);int64_t before=saved();TEST_ASSERT_TRUE(preview(255));uint32_t rev=settingsRevision();
    LittleFS.*flag=true;TEST_ASSERT_FALSE(commit());
    TEST_ASSERT_TRUE(settingsPreviewActive());TEST_ASSERT_EQUAL(rev,settingsRevision());TEST_ASSERT_EQUAL(before,saved());TEST_ASSERT_EQUAL(255,stripSettings[0].color);
    if(mainSurvives)TEST_ASSERT_EQUAL(base,fileColor(MAIN));else{TEST_ASSERT_FALSE(LittleFS.exists(MAIN));TEST_ASSERT_EQUAL(base,fileColor(BAK));}
    LittleFS.*flag=false;TEST_ASSERT_TRUE(commit());TEST_ASSERT_TRUE(isSaved());TEST_ASSERT_EQUAL(255,fileColor(MAIN));
}
void m4_commit_open_failure(){failedCommitKeepsEverything(&FakeFS::failWrite,true);}
void m4_commit_short_write(){failedCommitKeepsEverything(&FakeFS::shortWrite,true);}
void m4_commit_backup_rename_failure(){failedCommitKeepsEverything(&FakeFS::failBackup,true);}
void m4_commit_install_rename_failure(){failedCommitKeepsEverything(&FakeFS::failInstall,false);}
void m4_failed_commit_does_not_replace_existing_pending(){
    edit(255);uint32_t pendingRev=settingsRevision();TEST_ASSERT_TRUE(preview(65280));
    LittleFS.failWrite=true;TEST_ASSERT_FALSE(commit());LittleFS.failWrite=false;
    flushAt(1600);TEST_ASSERT_EQUAL(255,fileColor(MAIN));TEST_ASSERT_EQUAL(pendingRev,saved());TEST_ASSERT_TRUE(settingsPreviewActive());TEST_ASSERT_EQUAL(65280,stripSettings[0].color);
}
void m5_discard_to_saved_baseline_needs_no_write(){
    uint32_t writes=LittleFS.writes;TEST_ASSERT_TRUE(preview(255));drive.masterOn=true;uint32_t rev=settingsRevision();
    TEST_ASSERT_TRUE(discard());TEST_ASSERT_TRUE(settingsRevision()!=rev);TEST_ASSERT_TRUE(isSaved());TEST_ASSERT_TRUE(drive.masterOn);
    flushAt(5000);TEST_ASSERT_EQUAL(writes,LittleFS.writes);
}
void m5_expiry_to_saved_baseline_needs_no_write(){
    uint32_t writes=LittleFS.writes;TEST_ASSERT_TRUE(preview(255));drive.masterOn=true;
    flushAt(60000);TEST_ASSERT_FALSE(settingsPreviewActive());TEST_ASSERT_TRUE(isSaved());TEST_ASSERT_TRUE(drive.masterOn);
    flushAt(70000);TEST_ASSERT_EQUAL(writes,LittleFS.writes);
}
void m5_discard_to_pending_baseline_is_not_saved_until_written(){
    edit(255);int64_t old=saved();TEST_ASSERT_TRUE(preview(65280));uint32_t writes=LittleFS.writes;
    TEST_ASSERT_TRUE(discard());TEST_ASSERT_EQUAL(old,saved());TEST_ASSERT_FALSE(isSaved());TEST_ASSERT_EQUAL(255,stripSettings[0].color);
    flushAt(1600);TEST_ASSERT_EQUAL(writes+1,LittleFS.writes);TEST_ASSERT_TRUE(isSaved());TEST_ASSERT_EQUAL(255,fileColor(MAIN));
}
void m5_expiry_to_pending_baseline_is_not_saved_until_written(){
    edit(255);int64_t old=saved();testMillis=100;TEST_ASSERT_TRUE(preview(65280));LittleFS.failWrite=true;
    flushAt(60100);TEST_ASSERT_FALSE(settingsPreviewActive());TEST_ASSERT_EQUAL(old,saved());TEST_ASSERT_FALSE(isSaved());
    LittleFS.failWrite=false;flushAt(62000);TEST_ASSERT_TRUE(isSaved());TEST_ASSERT_EQUAL(255,fileColor(MAIN));
}
void m5_discard_after_pending_was_written_during_preview(){
    edit(255);TEST_ASSERT_TRUE(preview(65280));flushAt(1600);uint32_t writes=LittleFS.writes;TEST_ASSERT_FALSE(isSaved());
    TEST_ASSERT_TRUE(discard());TEST_ASSERT_TRUE(isSaved());flushAt(9000);TEST_ASSERT_EQUAL(writes,LittleFS.writes);
}
void m6_repeated_install_failures_keep_the_confirmed_copy(){
    int base=fileColor(MAIN);int64_t before=saved();LittleFS.failInstall=true;edit(255);
    for(uint32_t t=1600;t<=1600*4;t+=1600)flushAt(t);
    TEST_ASSERT_EQUAL(before,saved());TEST_ASSERT_FALSE(LittleFS.exists(MAIN));TEST_ASSERT_EQUAL(base,fileColor(BAK));
    reboot();TEST_ASSERT_EQUAL(base,stripSettings[0].color);TEST_ASSERT_TRUE(isSaved());
}
void m6_install_after_failure_then_reboot_loads_new(){
    LittleFS.failInstall=true;edit(255);flushAt(1600);LittleFS.failInstall=false;flushAt(3200);
    TEST_ASSERT_TRUE(isSaved());TEST_ASSERT_EQUAL(255,fileColor(MAIN));reboot();TEST_ASSERT_EQUAL(255,stripSettings[0].color);TEST_ASSERT_TRUE(isSaved());
}
void m6_corrupt_main_keeps_backup_through_failed_install(){
    int base=fileColor(MAIN);LittleFS.files[BAK]=LittleFS.files[MAIN];LittleFS.files[MAIN]=std::make_shared<std::string>("{broken");
    settingsBegin();TEST_ASSERT_TRUE(isSaved());LittleFS.failInstall=true;edit(255);flushAt(1600);
    TEST_ASSERT_EQUAL(base,fileColor(BAK));TEST_ASSERT_FALSE(isSaved());TEST_ASSERT_TRUE(saved()>=0);
    reboot();TEST_ASSERT_EQUAL(base,stripSettings[0].color);TEST_ASSERT_TRUE(isSaved());
}
void m6_lost_files_clear_the_marker(){
    edit(255);LittleFS.files.clear();LittleFS.failWrite=true;flushAt(1600);TEST_ASSERT_EQUAL(-1,saved());
}
void m7_presets_and_calibration_use_the_same_accounting(){
    uint32_t writes=LittleFS.writes;settingsPresetSave(0);TEST_ASSERT_FALSE(isSaved());flushAt(1600);TEST_ASSERT_TRUE(isSaved());TEST_ASSERT_EQUAL(writes+1,LittleFS.writes);
    edit(255);settingsPresetSave(1);flushAt(3200);edit(1);flushAt(4800);TEST_ASSERT_EQUAL(1,fileColor(MAIN));
    TEST_ASSERT_TRUE(settingsPresetLoad(1));TEST_ASSERT_EQUAL(255,stripSettings[0].color);TEST_ASSERT_FALSE(isSaved());flushAt(6400);TEST_ASSERT_TRUE(isSaved());TEST_ASSERT_EQUAL(255,fileColor(MAIN));
    drive.accOff[0]=.25f;settingsSave();TEST_ASSERT_FALSE(isSaved());flushAt(8000);TEST_ASSERT_TRUE(isSaved());
    DynamicJsonDocument d(24576);deserializeJson(d,*LittleFS.files[MAIN]);TEST_ASSERT_EQUAL_FLOAT(.25f,d["global"]["accOffset"][0].as<float>());
}
void m7_reads_touch_master_and_flush_do_not_advance(){
    uint32_t rev=settingsRevision(),writes=LittleFS.writes;
    settingsJson();settingsJson(true);settingsJson(false,true);drive.masterOn=!drive.masterOn;TEST_ASSERT_TRUE(settingsSaveNow());flushAt(5000);
    TEST_ASSERT_EQUAL(rev,settingsRevision());TEST_ASSERT_EQUAL(writes,LittleFS.writes);TEST_ASSERT_TRUE(isSaved());
    TEST_ASSERT_TRUE(preview(255));rev=settingsRevision();identity();String e;TEST_ASSERT_TRUE(settingsTouchPreview(request.as<JsonVariantConst>(),e));
    TEST_ASSERT_EQUAL(rev,settingsRevision());flushAt(9000);TEST_ASSERT_EQUAL(writes,LittleFS.writes);
}
void m7_saving_identical_content_does_not_write(){
    uint32_t writes=LittleFS.writes;settingsSave();TEST_ASSERT_TRUE(isSaved());flushAt(5000);TEST_ASSERT_EQUAL(writes,LittleFS.writes);
}
void m8_tokens_restart_per_boot_and_are_not_persisted(){
    edit(255);edit(254);flushAt(1600);uint32_t late=settingsRevision();TEST_ASSERT_TRUE(isSaved());
    reboot();TEST_ASSERT_TRUE(settingsRevision()<late);TEST_ASSERT_TRUE(isSaved());
    TEST_ASSERT_NULL(strstr(LittleFS.files[MAIN]->c_str(),"evision"));
    DynamicJsonDocument d(24576);deserializeJson(d,*LittleFS.files[MAIN]);TEST_ASSERT_EQUAL(5,d["schema"].as<int>());
}
int main(){UNITY_BEGIN();RUN_TEST(m1_defaults_without_file_are_not_saved_and_not_written); RUN_TEST(m1_schema5_main_and_backup_load_as_saved); RUN_TEST(m1_unavailable_fs_is_never_saved); RUN_TEST(m1_migration_is_saved_only_after_its_write); RUN_TEST(m1_failed_migration_write_stays_unsaved); RUN_TEST(m2_change_is_saved_only_after_the_delayed_write); RUN_TEST(m3_old_pending_written_during_preview_keeps_current_dirty); RUN_TEST(m4_commit_success_reports_consistent_revisions); RUN_TEST(m4_commit_open_failure); RUN_TEST(m4_commit_short_write); RUN_TEST(m4_commit_backup_rename_failure); RUN_TEST(m4_commit_install_rename_failure); RUN_TEST(m4_failed_commit_does_not_replace_existing_pending); RUN_TEST(m5_discard_to_saved_baseline_needs_no_write); RUN_TEST(m5_expiry_to_saved_baseline_needs_no_write); RUN_TEST(m5_discard_to_pending_baseline_is_not_saved_until_written); RUN_TEST(m5_expiry_to_pending_baseline_is_not_saved_until_written); RUN_TEST(m5_discard_after_pending_was_written_during_preview); RUN_TEST(m6_repeated_install_failures_keep_the_confirmed_copy); RUN_TEST(m6_install_after_failure_then_reboot_loads_new); RUN_TEST(m6_corrupt_main_keeps_backup_through_failed_install); RUN_TEST(m6_lost_files_clear_the_marker); RUN_TEST(m7_presets_and_calibration_use_the_same_accounting); RUN_TEST(m7_reads_touch_master_and_flush_do_not_advance); RUN_TEST(m7_saving_identical_content_does_not_write); RUN_TEST(m8_tokens_restart_per_boot_and_are_not_persisted);RUN_TEST(no_preview_leak_to_pending_or_backup);RUN_TEST(revision_and_second_client);RUN_TEST(expiry_and_manual_discard);RUN_TEST(flash_failure_is_not_a_save);RUN_TEST(invalid_patch_atomic);RUN_TEST(failed_install_recovers_backup_without_preview);RUN_TEST(legacy_speed_and_order_migration);RUN_TEST(lease_touch_wrap);return UNITY_END();}
