#ifndef NET_H
#define NET_H

#include <Arduino.h>

// AP primary; bounded boot attempts / explicit STA reconnect, no background scans
extern String wifiSsid;
extern String wifiPass;

struct KnownNet { const char* ssid; const char* pass; };
const char* netDefaultSsid();
const char* netDefaultPass();

void netBegin();
void netTick();
void netReconnect();
bool netApOn();
uint32_t netDroppedEvents();
const char* netPhase();
uint32_t netAttempts();
uint8_t netLastDisconnectReason();
String netStatusString();  // mode + ssid + ip
String netStaString();     // ip + rssi or "-"
String netApString();      // ap ssid + ip or "off"

#endif
