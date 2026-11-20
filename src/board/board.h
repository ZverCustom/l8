#pragma once
#include <Arduino.h>
#include <Wire.h>
void boardBegin();
void boardLock();
void boardUnlock();
bool boardRead(uint8_t address, uint8_t reg, uint8_t* data, uint8_t count);
bool boardWrite(uint8_t address, uint8_t reg, uint8_t value);
bool expWrite(uint8_t reg, uint16_t value);
bool expRead(uint8_t reg, uint16_t& value);
bool expSafeInit();
void boardDataMask(uint8_t mask); // physical GPIO11..18 / PCA P00..P07 bits, not UI channel order
uint8_t boardDataMask();
