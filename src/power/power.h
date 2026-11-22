#pragma once
#include <Arduino.h>
struct ChannelPower { float brightnessScale=1; uint8_t retries=0; uint32_t retryInMs=0; bool retryPending=false; float amps=0, isVolts=0; bool on=false; uint8_t fault=0, reason=1 /* io until the power task reports */; uint32_t sampleAt=0; };
struct PowerState {
    float vbat=0, ledAmps=0, isVolts=0, totalBrightnessScale=1;
    bool inhibited=false;
    bool ledPowerAllowed=false; // supply / IGN / inhibit / I2C; Master and channel faults are separate
    bool on=false, latched=false, ign=false, hold=false, ioOk=false, ioLatched=false;
    bool sensorOn=false, sensorFault=false;
    ChannelPower channels[8];
    float inputMv[3]={}, inputValue[3]={};
    uint32_t sampleAt=0;
};
extern PowerState power; // main-loop snapshot; use powerSnapshot() from other tasks
void powerBegin();
void powerTick();
void powerSampleAdc(); // kept as no-op: ADC runs in the safety task on core 0
void powerResetLatch(int channel=-1);
void powerInhibit(bool inhibit);
void powerCanSilent(bool silent);
PowerState powerSnapshot();
String powerStatusString();
const char* powerFaultName(uint8_t fault);
const char* powerReasonName(uint8_t reason);
