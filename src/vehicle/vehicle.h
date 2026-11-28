#ifndef VEHICLE_H
#define VEHICLE_H

#include <Arduino.h>
#include "can_bus/can_bus.h"
#include "obd2/obd2.h"

// CAN data source
enum CanMode : int {
    CAN_MODE_OBD2 = 0,     // active OBD2 PID polling
    CAN_MODE_BMW = 1,      // passive decode of BMW MS43 PT-CAN
    CAN_MODE_SNIFFER = 2,  // passive, dump all IDs to Serial
    CAN_MODE_DS2 = 3,      // BMW MS43 over K-line, CAN stays listen-only
    CAN_MODE_PANEL = 4,    // D10 panel bus, 1 Mbit/s (v6 §15, task 036)
};

struct VehicleData {
    uint16_t rpm = 0;
    uint8_t speed = 0;
    uint8_t throttle = 0;
    int8_t temp = 0;
    unsigned long lastUpdate = 0;  // millis of last decoded value
};

extern VehicleData vehicle;
// Panel mode: §14.4 freshness per field (bits of panel_core::kFresh*: rpm, speed, throttle, temp).
extern uint8_t vehicleFresh;
extern int canMode;
extern int canBitrateSel;   // 0 = auto, else CanBitrate + 1 (not persisted)

void vehicleSetBitrate(int sel);
String canBitrateString();

void vehicleSetup(CANBus& can, OBD2& obd);
void vehicleSetMode(int mode);
void vehicleTick();
bool vehicleDataFresh();
bool engineRunning();
// A field (0 rpm, 1 speed, 2 throttle, 3 temp) is current: panel mode by its own bit, other
// modes by vehicleDataFresh() as before (vehicle_rules.h).
bool vehicleFieldFresh(int field);
// The temperature may be read (panel mode: only when fresh; other modes: always, as before).
bool vehicleTempKnown();
String canStatusString();
String sniffTableString();

#endif
