#ifndef SETTINGS_H
#define SETTINGS_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include "led_control/led_control.h"

#define NUM_PRESETS 3
#define L8_CONFIG_SCHEMA 5
#define L8_MAX_CONFIG_BYTES 24000
#define L8_PREVIEW_LEASE_MS 60000
#define L8_MAX_CHANNEL_AMPS 5

struct DriveSettings {
    bool masterOn;        // global output switch
    int shiftRPM;         // blink above this RPM (0 = use maxRPM)
    uint32_t shiftColor;
    int engineOff;        // when no data / rpm 0: 0 keep, 1 dim, 2 off
    int engineOffDim;     // brightness % used for engineOff == 1
    int powerLimitMa;     // measured aggregate current cap, 0 = off
    int lateralAxis;      // 0 +X, 1 -X, 2 +Y, 3 -Y, 4 +Z, 5 -Z
    int yawAxis;          // same encoding, gyro axis for yaw rate
    int maxSpeed;         // km/h full scale for speed source
    float maxLateralG;    // full scale for lateral G source
    float maxYawRate;     // deg/s full scale for yaw source
    float accOff[3];      // calibration offsets
    float gyrOff[3];
    float ledMaxAmps;     // LED supply switch cuts above this
};

struct Preset {
    bool used;
    char name[17];
    StripSettings strips[NUM_STRIPS];
    int numLeds[NUM_STRIPS];
    CanSettings can;
};

struct InputSettings { int mode; float low, high, thresholdMv; };

struct SavedData {
    bool autoStart;
    bool allowWithoutIgn; // strip power policy; never gates controller HOLD or peripherals
    bool autoRetry;
    bool sensor5v;
    int bitrate;
    float channelMaxAmps[8];
    float channelCal[8];
    InputSettings inputs[3];
    CanSettings can_setting;
    StripSettings stripSettings[NUM_STRIPS];
    int numLeds[NUM_STRIPS];
    char wifiSsid[33];
    char wifiPass[65];
    int canMode;
    DriveSettings drive;
    Preset presets[NUM_PRESETS];
    float currentCal;     // LED current = IS reading * this; appended so old files keep their data
    bool cutPowerEngineOff;  // engineOff == Off also opens the strip switch (no WS2815 idle draw)
};

extern SavedData savedData;
extern DriveSettings& drive;

void settingsBegin();       // mount FS, load file, push into live globals
void settingsSave();        // live globals -> savedData, schedule write
bool settingsSaveNow();
bool settingsPreview(JsonVariantConst request, bool full, String& error);
bool settingsCommitPreview(JsonVariantConst request, String& error);
bool settingsTouchPreview(JsonVariantConst request, String& error);
bool settingsDiscardPreview(JsonVariantConst request, String& error);
bool settingsPreviewActive();
uint32_t settingsRevision();
// Revision whose content is confirmed in flash; false (JSON null) when none is.
bool settingsSavedRevision(uint32_t& value);
void settingsTick();
void settingsPresetSave(int i);
bool settingsPresetLoad(int i);
String settingsPresetName(int i);

String settingsJson(bool secrets=false, bool baseline=false);
bool settingsApply(JsonVariantConst config, String& error);
bool settingsHealthy();

#endif
