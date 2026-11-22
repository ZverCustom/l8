#include <Arduino.h>
#include "board/board.h"
#include "settings/settings.h"
#include "led_control/led_control.h"
#include "obd2/obd2.h"
#include "can_bus/can_bus.h"
#include "hub/hub.h"
#include "net/net.h"
#include "vehicle/vehicle.h"
#include "imu/imu.h"
#include "system/system.h"
#include "power/power.h"
#include "kline/kline.h"

CANBus canBus;
OBD2 obd2;

void setup() {
    boardBegin();
    // USB hosts send a complete JSON configuration in one burst. The Arduino
    // default 256-byte RX queue otherwise drops its tail (including newline).
    Serial.setRxBufferSize(32768);
    Serial.begin(115200);
    Serial.setTxTimeoutMs(100);
    delay(2000);
    Serial.println("System initializing...");

    settingsBegin();
    powerInhibit(true);
    powerBegin();
    sysBegin();
    initializeLEDStrips();

    twai_timing_config_t timingConfig = TWAI_TIMING_CONFIG_500KBITS();
    canBus.begin(CAN_RX_PIN, CAN_TX_PIN, CAN_STB_PIN, timingConfig, canMode != CAN_MODE_OBD2);
    obd2.begin(canBus);
    vehicleSetup(canBus, obd2);
    vehicleSetBitrate(canBitrateSel);
    klineBegin();

    netBegin();
    setupHub();
    imuStartTask();
    startLedTask();
    sysLog(F("ready"));
}

void loop() {
    powerTick();
    netTick();
    settingsTick();
    sysTick();
    updateHub();
    vehicleTick();
    powerTick();
    delay(1);
}
