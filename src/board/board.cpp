#include "board.h"
#include "pins/pins.h"
#include <v3_safety.h>
#include <driver/gpio.h>
#include <rom/gpio.h>
#include <soc/gpio_sig_map.h>
static SemaphoreHandle_t busMutex;
static portMUX_TYPE routeMutex = portMUX_INITIALIZER_UNLOCKED;
static volatile uint8_t dataMask = 0;
void boardLock() { xSemaphoreTakeRecursive(busMutex, portMAX_DELAY); }
void boardUnlock() { xSemaphoreGiveRecursive(busMutex); }
void boardDataMask(uint8_t mask) {
    portENTER_CRITICAL(&routeMutex);
    dataMask = mask;
    for (int i=0; i<8; ++i) if (!(mask & (1u<<i))) {
        gpio_matrix_out(LED_PIN_1+i, SIG_GPIO_OUT_IDX, false, false);
        gpio_set_level((gpio_num_t)(LED_PIN_1+i), 0);
    }
    portEXIT_CRITICAL(&routeMutex);
}
uint8_t boardDataMask() { return dataMask; }
// Called by the pinned FastLED RMT driver, including from its ISR. The same
// critical section disconnects DATA before a switch is turned off, so a late
// RMT channel dispatch cannot reconnect an unpowered strip.
extern "C" void IRAM_ATTR l8_rmt_route(int channel, int pin) {
    portENTER_CRITICAL_ISR(&routeMutex);
    int index = pin - LED_PIN_1;
    bool on = index >= 0 && index < 8 && (dataMask & (1u << index));
    gpio_matrix_out(pin, on ? RMT_SIG_OUT0_IDX + channel : SIG_GPIO_OUT_IDX, false, false);
    if (!on) gpio_set_level((gpio_num_t)pin, 0);
    portEXIT_CRITICAL_ISR(&routeMutex);
}
void boardBegin() {
    gpio_set_level((gpio_num_t)HOLD_PIN, 0);
    gpio_set_direction((gpio_num_t)HOLD_PIN, GPIO_MODE_INPUT_OUTPUT);
    for (int i=0; i<8; ++i) {
        gpio_set_level((gpio_num_t)(LED_PIN_1+i), 0);
        gpio_set_direction((gpio_num_t)(LED_PIN_1+i), GPIO_MODE_OUTPUT);
    }
    boardDataMask(0);
    pinMode(IGN_PIN, INPUT);
    pinMode(SENSOR_FAULT_PIN, INPUT);
    for (int pin : INPUT_PULL_PINS) pinMode(pin, INPUT);
    busMutex = xSemaphoreCreateRecursiveMutex();
    Wire.begin(IMU_SDA_PIN, IMU_SCL_PIN, 400000);
    Wire.setTimeOut(10);
}
bool boardRead(uint8_t address, uint8_t reg, uint8_t* data, uint8_t count) {
    boardLock();
    Wire.beginTransmission(address); Wire.write(reg);
    bool ok = Wire.endTransmission(false) == 0;
    if (ok) ok = Wire.requestFrom(address, count) == count;
    if (ok) for (int i=0;i<count;++i) data[i]=Wire.read();
    boardUnlock(); return ok;
}
bool boardWrite(uint8_t address, uint8_t reg, uint8_t value) {
    boardLock(); Wire.beginTransmission(address); Wire.write(reg); Wire.write(value);
    bool ok = Wire.endTransmission() == 0; boardUnlock(); return ok;
}
bool expWrite(uint8_t reg, uint16_t value) {
    boardLock(); Wire.beginTransmission(0x20); Wire.write(reg);
    Wire.write((uint8_t)value); Wire.write((uint8_t)(value>>8));
    bool ok = Wire.endTransmission() == 0; boardUnlock(); return ok;
}
bool expRead(uint8_t reg, uint16_t& value) {
    uint8_t b[2]; if (!boardRead(0x20, reg, b, 2)) return false;
    value = b[0] | (uint16_t(b[1])<<8); return true;
}
bool expSafeInit() {
    uint16_t value=0xffff;
    boardLock();
    bool ok = expWrite(2,v3::SAFE) && expRead(2,value) && value==v3::SAFE &&
              expWrite(4,0) && expWrite(6,0) && expRead(6,value) && value==0;
    boardUnlock(); return ok;
}
