#include "power.h"
#include "board/board.h"
#include "pins/pins.h"
#include "settings/settings.h"
#include "vehicle/vehicle.h"
#include "vehicle/vehicle_rules.h"
#include <v3_safety.h>
#include <channel_reason.h>
static_assert(uint8_t(v3::Reason::Io)==1,"ChannelPower::reason defaults to io");
#include <driver/gpio.h>
#include <current_control.h>
struct Request {
    float totalMaxAmps=0; bool autoRetry=true;
    bool master=false, masterRequested=false, engineCut=false, allowWithoutIgn=false, sensor=false, inhibit=false, silent=true;
    bool enabled[8]={};
    float maxAmps[8]={}, calibration[8]={};
    int inputMode[3]={}; float low[3]={}, high[3]={}, threshold[3]={};
};
static Request request;
static PowerState state;
PowerState power;
static portMUX_TYPE stateMux=portMUX_INITIALIZER_UNLOCKED;
static uint16_t resetMask=0;
static l8power::Channel protection[8];
static l8power::Dimmer totalDimmer;
PowerState powerSnapshot() { portENTER_CRITICAL(&stateMux); auto s=state; portEXIT_CRITICAL(&stateMux); return s; }
void powerInhibit(bool value) { portENTER_CRITICAL(&stateMux); request.inhibit=value; portEXIT_CRITICAL(&stateMux); if(value) boardDataMask(0); }
void powerCanSilent(bool value) { portENTER_CRITICAL(&stateMux); request.silent=value; portEXIT_CRITICAL(&stateMux); }
void powerResetLatch(int ch) { portENTER_CRITICAL(&stateMux); resetMask |= ch<0 ? 0x1ff : (ch<8 ? (1u<<ch) : 0); portEXIT_CRITICAL(&stateMux); }
void powerSampleAdc() {}
const char* powerReasonName(uint8_t v) { return v3::reasonName(v3::Reason(v)); }
const char* powerFaultName(uint8_t v) { return v==1 ? "overcurrent" : v==2 ? "switch fault" : v==4 ? "invalid sample" : ""; }
void powerTick() {
    Request next;
    next.master=drive.masterOn; next.autoRetry=savedData.autoRetry; next.totalMaxAmps=drive.powerLimitMa/1000.f;
    bool parked=vrules::parkedCut(savedData.cutPowerEngineOff, drive.engineOff, engineRunning());
    next.masterRequested=drive.masterOn; next.engineCut=parked;
    if(parked) next.master=false;
    next.allowWithoutIgn=savedData.allowWithoutIgn; next.sensor=savedData.sensor5v;
    for(int i=0;i<8;++i) { next.enabled[i]=stripSettings[i].enabled; next.maxAmps[i]=savedData.channelMaxAmps[i]; next.calibration[i]=savedData.channelCal[i]; }
    for(int i=0;i<3;++i) { next.inputMode[i]=savedData.inputs[i].mode; next.low[i]=savedData.inputs[i].low; next.high[i]=savedData.inputs[i].high; next.threshold[i]=savedData.inputs[i].thresholdMv; }
    portENTER_CRITICAL(&stateMux);
    next.inhibit=request.inhibit; next.silent=request.silent; request=next; power=state;
    portEXIT_CRITICAL(&stateMux);
}
static void safetyTask(void*) {
    PowerState s;
    bool initOk=expSafeInit(); s.ioOk=initOk; s.ioLatched=!initOk;
    int previousModes[3]={-1,-1,-1};
    uint32_t ignChanged=millis(), lastCheck=0;
    bool rawPrevious=digitalRead(IGN_PIN);
    uint32_t enabledAt[8]={};
    for(;;) {
        uint32_t now=millis(); Request cfg; uint16_t reset;
        portENTER_CRITICAL(&stateMux); cfg=request; reset=resetMask; resetMask=0; portEXIT_CRITICAL(&stateMux);
        s.vbat=v3::vbatFromAdc(analogReadMilliVolts(VBAT_SENSE_PIN)/1000.f);
        bool raw=digitalRead(IGN_PIN);
        if(raw!=rawPrevious) { rawPrevious=raw; ignChanged=now; }
        if(now-ignChanged>=20) s.ign=raw;
        // IGN controls strip policy only. Once started, keep the controller alive
        // for settings/diagnostics even when IGN, Master or the outputs are off.
        // The v3 hardware still needs USB or IGN to start the buck from cold.
        s.hold=v3::controllerHold(s.vbat);
        gpio_set_level((gpio_num_t)HOLD_PIN, s.hold);
        // Resets require Master OFF; restoring I2C never resumes loads automatically.
        if(reset && !cfg.master) {
            for(int i=0;i<8;++i) if(reset&(1u<<i)) { s.channels[i].fault=0; protection[i].reset(); }
            if(reset&0x100) { s.sensorFault=false; s.ioLatched=false; initOk=expSafeInit(); if(!initOk) s.ioLatched=true; }
        }
        if(!initOk && now-lastCheck>=500) { lastCheck=now; initOk=expSafeInit(); }
        if(now-lastCheck>=500 && initOk) {
            lastCheck=now; uint16_t config=0xffff;
            if(!expRead(6,config) || config!=0) { initOk=false; s.ioLatched=true; }
        }
        bool allowed=v3::outputsAllowed(s.vbat,cfg.inhibit,initOk,s.ioLatched);
        s.ledPowerAllowed=v3::ledsAllowed(s.vbat,s.ign,cfg.allowWithoutIgn,cfg.inhibit,initOk,s.ioLatched);
        uint8_t mask=0; bool rising[8]={};
        for(int i=0;i<8;++i) {
            bool permitted=s.ledPowerAllowed && cfg.master && cfg.enabled[i];
            bool on=protection[i].ready(now,cfg.autoRetry,permitted) && permitted;
            s.channels[i].fault=protection[i].fault;
            if(on && !s.channels[i].on) { rising[i]=true; enabledAt[i]=now; protection[i].off(); }
            s.channels[i].on=on;
            if(on) mask|=v3::channelMask(i);
            else { s.channels[i].amps=0; s.channels[i].isVolts=0; protection[i].off(); }
        }
        // Disconnect before changing supply; newly enabled strips remain low for 3 ms.
        uint8_t data=mask;
        for(int i=0;i<8;++i) if(now-enabledAt[i]<3) data&=~v3::channelMask(i);
        boardDataMask(data);
        if(!digitalRead(SENSOR_FAULT_PIN) && s.sensorOn) s.sensorFault=true;
        s.sensorOn=allowed && cfg.sensor && !s.sensorFault;
        uint16_t base=mask | ((cfg.silent || !allowed) ? 0x2000 : 0) |
                      ((now/500&1) ? 0x4000 : 0) | (s.sensorOn ? 0x8000 : 0);
        bool ok=initOk && expWrite(2,base); // all DEN low between channels
        if(ok) for(int i=0;i<8;++i) if(rising[i]) { enabledAt[i]=millis(); protection[i].enabled(enabledAt[i]); }
        s.ledAmps=0; s.latched=s.ioLatched || s.sensorFault;
        for(int i=0;i<8 && ok;++i) {
            auto& ch=s.channels[i];
            if(!ch.on) { s.latched |= ch.fault!=0; continue; }
            // Break before make: only one DEN is ever high; allow 100 us settling
            // (datasheet max 60 us after DEN/DSEL in small-load mode).
            ok=expWrite(2,base) && expWrite(2,base|v3::senseSelect(v3::hardwareChannel(i)));
            if(!ok) break;
            delayMicroseconds(100);
            ch.isVolts=analogReadMilliVolts(LED_ISENSE_PIN)/1000.f;
            ch.amps=v3::currentAmps(ch.isVolts)*cfg.calibration[i]; ch.sampleAt=millis();
            protection[i].sample(ch.sampleAt,ch.amps,ch.isVolts,cfg.maxAmps[i],cfg.autoRetry);
            if(protection[i].fault!=l8power::Channel::None) {
                ch.fault=protection[i].fault; ch.on=false;
                mask &= ~v3::channelMask(i); base &= ~v3::channelMask(i); boardDataMask(boardDataMask() & mask);
                s.latched=true;
            }
            s.ledAmps+=ch.amps;
        }
        if(ok) ok=expWrite(2,base);
        totalDimmer.update(now,s.ledAmps,cfg.totalMaxAmps);
        s.totalBrightnessScale=totalDimmer.scale;
        for(int i=0;i<8;++i) { auto& ch=s.channels[i]; const auto& c=protection[i]; ch.brightnessScale=c.dimmer.scale; ch.retries=c.retries; ch.retryPending=c.waiting; ch.retryInMs=c.remaining(now); }
        if(!ok) {
            boardDataMask(0); s.ioLatched=true; s.latched=true; initOk=false;
            s.ledPowerAllowed=false;
            expWrite(2,v3::SAFE); // best effort: broken I2C cannot guarantee physical IN levels
            for(auto& ch:s.channels) ch.on=false;
            s.sensorOn=false;
        }
        s.ioOk=ok;
        // Diagnostic only, from the inputs this cycle used; switching above is unchanged.
        for(int i=0;i<8;++i) {
            auto& ch=s.channels[i];
            v3::ReasonInputs r{ok && initOk && !s.ioLatched, protection[i].fault!=l8power::Channel::None, protection[i].waiting,
                cfg.inhibit, v3::supplyValid(s.vbat), s.ign||cfg.allowWithoutIgn, cfg.masterRequested, cfg.enabled[i], cfg.engineCut, ch.on};
            ch.reason=uint8_t(v3::channelReason(r));
        }
        for(int i=0;i<3;++i) {
            if(previousModes[i]!=cfg.inputMode[i]) {
                previousModes[i]=cfg.inputMode[i];
                if(cfg.inputMode[i]==0) pinMode(INPUT_PULL_PINS[i],INPUT);
                else { digitalWrite(INPUT_PULL_PINS[i],cfg.inputMode[i]==2); pinMode(INPUT_PULL_PINS[i],OUTPUT); }
            }
            s.inputMv[i]=analogReadMilliVolts(INPUT_SENSE_PINS[i]);
            s.inputValue[i]=v3::inputFraction(s.inputMv[i],cfg.inputMode[i],cfg.low[i],cfg.high[i],cfg.threshold[i]);
        }
        s.on=false; for(auto& ch:s.channels) s.on |= ch.on;
        s.inhibited=cfg.inhibit;
        s.sampleAt=millis();
        portENTER_CRITICAL(&stateMux); state=s; portEXIT_CRITICAL(&stateMux);
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}
void powerBegin() {
    analogReadResolution(12);
    analogSetPinAttenuation(VBAT_SENSE_PIN,ADC_11db);
    analogSetPinAttenuation(LED_ISENSE_PIN,ADC_6db);
    for(int pin:INPUT_SENSE_PINS) analogSetPinAttenuation(pin,ADC_11db);
    powerTick();
    xTaskCreatePinnedToCore(safetyTask,"power",4096,nullptr,3,nullptr,0);
}
String powerStatusString() {
    auto s=powerSnapshot();
    if(s.ioLatched) return "I2C fault: outputs unconfirmed; reset with Master off";
    if(!v3::supplyValid(s.vbat)) return "USB / supply outside 9-16.5 V";
    if(s.inhibited) return "outputs inhibited";
    if(!s.ign && !savedData.allowWithoutIgn) return "strips waiting for IGN";
    if(s.latched) return "channel / sensor protection";
    return s.on ? "on" : "off";
}
