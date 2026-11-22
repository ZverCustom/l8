#pragma once
#include <Arduino.h>
constexpr int CAN_MODE_SNIFFER=2;
extern int canMode,canBitrateSel;
void vehicleSetMode(int);
void vehicleSetBitrate(int);
