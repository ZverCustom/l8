// Firmware glue of the L8 panel mode (task 036): PanelMode with the TWAI port, NVS, the
// shared l8proto dispatcher, STATUS_A from settings/power, the vehicle view into `vehicle`.
#pragma once

#include <Arduino.h>

void panelModeEnter();
void panelModeExit();
bool panelModeActive();
void panelModeTick();
String panelModeStatus();
