#ifndef SYSTEM_H
#define SYSTEM_H

#include <Arduino.h>

// Concurrent RAM journal with batched LittleFS persistence
void sysBegin();                 // call after LittleFS is mounted
void sysLog(const String& msg);  // Serial + RAM; no flash writes
String sysLogTail(int maxBytes = 2500);
void sysClearLog();
void sysTick();
void sysFlush();
String sysResetReason();
String sysUptime();
String sysBuildInfo();

#endif
