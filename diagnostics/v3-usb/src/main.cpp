#include <Arduino.h>
#include <Wire.h>
#include <driver/gpio.h>
#include <esp_system.h>
#include <esp32s3/spiram.h>

// Source: PCB/v3/gen/pinmap.py. This program has no command that enables a load.
namespace Board {
constexpr uint8_t hold = 41;
constexpr uint8_t ign = 40;
constexpr uint8_t vbat = 1;
constexpr uint8_t isense = 2;
constexpr uint8_t fault5v = 47;
constexpr uint8_t sda = 8;
constexpr uint8_t scl = 9;
constexpr uint8_t ledData[] = {11, 12, 13, 14, 15, 16, 17, 18};
constexpr uint8_t inputSense[] = {4, 5, 6};
constexpr uint8_t inputPull[] = {7, 10, 21};
constexpr uint8_t expander = 0x20;
constexpr uint8_t imu = 0x6A;
// P15 = CAN_S high (silent). All LED IN, DEN, DSEL, status and 5V_SENS_EN low.
constexpr uint16_t safeOutputs = 0x2000;
}

static bool expanderReady = false;
static bool imuReady = false;
static bool psramTestOk = false;
static uint32_t psramChipBytes = 0;
static uint32_t lastReport = 0;

static void outputLow(uint8_t pin) {
    // Preload the output latch before enabling its driver.
    gpio_set_level(static_cast<gpio_num_t>(pin), 0);
    gpio_set_direction(static_cast<gpio_num_t>(pin), GPIO_MODE_INPUT_OUTPUT);
}

static bool readRegs(uint8_t address, uint8_t reg, uint8_t* data, uint8_t count) {
    Wire.beginTransmission(address);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom(address, count) != count) return false;
    for (uint8_t i = 0; i < count; ++i) data[i] = Wire.read();
    return true;
}

static bool writeReg(uint8_t address, uint8_t reg, uint8_t value) {
    Wire.beginTransmission(address);
    Wire.write(reg);
    Wire.write(value);
    return Wire.endTransmission() == 0;
}

static bool writeExpander(uint8_t reg, uint16_t value) {
    Wire.beginTransmission(Board::expander);
    Wire.write(reg);
    Wire.write(static_cast<uint8_t>(value));
    Wire.write(static_cast<uint8_t>(value >> 8));
    return Wire.endTransmission() == 0;
}

static bool readExpander(uint8_t reg, uint16_t& value) {
    uint8_t data[2];
    if (!readRegs(Board::expander, reg, data, sizeof(data))) return false;
    value = data[0] | (static_cast<uint16_t>(data[1]) << 8);
    return true;
}

static bool initExpander() {
    // PCA9555 resets as inputs with HIGH output latches. Never enable those
    // defaults: write and verify the safe latches BEFORE changing direction.
    uint16_t output = 0;
    if (!writeExpander(0x02, Board::safeOutputs) ||
        !readExpander(0x02, output) || output != Board::safeOutputs) return false;
    if (!writeExpander(0x04, 0x0000)) return false;  // no input polarity inversion
    if (!writeExpander(0x06, 0x0000)) return false;  // all pins are now outputs
    uint16_t config = 0xFFFF;
    return readExpander(0x06, config) && config == 0x0000;
}

static bool initImu() {
    uint8_t who = 0;
    if (!readRegs(Board::imu, 0x0F, &who, 1) || who != 0x6A) return false;
    if (!writeReg(Board::imu, 0x12, 0x01)) return false;
    // Wait for the sensor's software reset, with a bounded timeout.
    bool resetDone = false;
    for (uint8_t i = 0; i < 20; ++i) {
        delay(5);
        uint8_t control = 0;
        if (!readRegs(Board::imu, 0x12, &control, 1)) return false;
        if (!(control & 0x01)) { resetDone = true; break; }
    }
    if (!resetDone) return false;
    return writeReg(Board::imu, 0x12, 0x44) &&  // BDU and register auto-increment
           writeReg(Board::imu, 0x10, 0x48) &&  // accelerometer: 104 Hz, +/-4 g
           writeReg(Board::imu, 0x11, 0x44);    // gyro: 104 Hz, +/-500 deg/s
}

static bool testPsram() {
    // Small scratch test, not a full memory qualification.
    constexpr size_t bytes = 64 * 1024;
    auto* memory = static_cast<volatile uint32_t*>(ps_malloc(bytes));
    if (!memory) return false;
    bool ok = true;
    for (uint32_t pattern : {0xA5A55A5Au, 0x5A5AA5A5u}) {
        for (size_t i = 0; i < bytes / 4; ++i) memory[i] = pattern ^ i;
        for (size_t i = 0; i < bytes / 4; ++i) {
            if (memory[i] != (pattern ^ i)) { ok = false; break; }
        }
        if (!ok) break;
    }
    free(const_cast<uint32_t*>(memory));
    return ok;
}

static void printReport() {
    Serial.printf("\nL8 v3 USB diagnostic | uptime=%lus | reset=%d\n",
                  millis() / 1000UL, static_cast<int>(esp_reset_reason()));
    Serial.printf("ESP32: %s rev=%u cores=%u | flash=%u MB (expected 16)\n",
                  ESP.getChipModel(), ESP.getChipRevision(), ESP.getChipCores(),
                  ESP.getFlashChipSize() / (1024 * 1024));
    Serial.printf("PSRAM: chip=%u KB (expected 2048), capacity=%s, usable=%u KB, 64 KB scratch test=%s | heap=%u\n",
                  psramChipBytes / 1024, psramChipBytes == 2 * 1024 * 1024 ? "PASS" : "FAIL",
                  ESP.getPsramSize() / 1024, psramTestOk ? "PASS" : "FAIL",
                  ESP.getFreeHeap());
    Serial.printf("GPIO: HOLD=%d (must be 0), IGN=%d, 5V_FAULT_N=%d | I2C SDA=%d SCL=%d\n",
                  digitalRead(Board::hold), digitalRead(Board::ign), digitalRead(Board::fault5v),
                  digitalRead(Board::sda), digitalRead(Board::scl));
    Serial.printf("ADC: VBAT=%.2f V (USB only: near 0), IS=%u mV (DEN off) | IN=%u,%u,%u mV\n",
                  analogReadMilliVolts(Board::vbat) * 0.0057f,
                  analogReadMilliVolts(Board::isense),
                  analogReadMilliVolts(Board::inputSense[0]),
                  analogReadMilliVolts(Board::inputSense[1]),
                  analogReadMilliVolts(Board::inputSense[2]));

    uint16_t output = 0, config = 0, inputs = 0;
    bool readOk = readExpander(0x02, output) && readExpander(0x06, config) &&
                  readExpander(0x00, inputs);
    if (!readOk) {
        Serial.println("PCA9555 @0x20: I2C READ FAIL");
    } else {
        bool safe = expanderReady && output == Board::safeOutputs &&
                    config == 0 && inputs == Board::safeOutputs;
        Serial.printf("PCA9555 @0x20: %s OUT=0x%04X CFG=0x%04X PIN=0x%04X\n",
                      safe ? "PASS" : "FAIL (inspect registers/pins)", output, config, inputs);
    }

    uint8_t who = 0, status = 0;
    if (!readRegs(Board::imu, 0x0F, &who, 1)) {
        Serial.println("LSM6DS3TR-C @0x6A: I2C READ FAIL");
    } else {
        Serial.printf("LSM6DS3TR-C @0x6A: WHO=0x%02X (expected 0x6A), init=%s\n",
                      who, imuReady ? "PASS" : "FAIL");
        uint8_t data[14];
        if (imuReady && who == 0x6A && readRegs(Board::imu, 0x1E, &status, 1) &&
            (status & 0x03) == 0x03 && readRegs(Board::imu, 0x20, data, sizeof(data))) {
            int16_t raw[7];
            for (uint8_t i = 0; i < 7; ++i)
                raw[i] = static_cast<int16_t>(data[2 * i] | (data[2 * i + 1] << 8));
            Serial.printf("IMU: temp=%.1f C accel=%.3f,%.3f,%.3f g gyro=%.2f,%.2f,%.2f dps\n",
                          raw[0] / 256.0f + 25.0f,
                          raw[4] * 0.000122f, raw[5] * 0.000122f, raw[6] * 0.000122f,
                          raw[1] * 0.0175f, raw[2] * 0.0175f, raw[3] * 0.0175f);
        } else {
            Serial.println("IMU: no fresh sample");
        }
    }
    Serial.println("Requested mode: USB only; LED power/DATA, DEN and sensor 5V OFF; CAN silent; HOLD LOW.");
}

void setup() {
    outputLow(Board::hold);
    for (uint8_t pin : Board::ledData) outputLow(pin);
    for (uint8_t pin : Board::inputPull) pinMode(pin, INPUT);
    for (uint8_t pin : Board::inputSense) pinMode(pin, INPUT);
    pinMode(Board::ign, INPUT);
    pinMode(Board::fault5v, INPUT);
    // CAN TX/RX, K-line and UART0 remain unconfigured; no bus transmissions.
    Serial.begin(115200);
    Wire.begin(Board::sda, Board::scl, 100000);
    Wire.setTimeOut(20);
    expanderReady = initExpander();
    imuReady = initImu();
    analogReadResolution(12);
    analogSetPinAttenuation(Board::vbat, ADC_11db);
    analogSetPinAttenuation(Board::isense, ADC_6db);
    for (uint8_t pin : Board::inputSense) analogSetPinAttenuation(pin, ADC_11db);
    // Arduino getPsramSize() is usable heap, which excludes allocator metadata.
    // Check physical capacity separately so the scratch test always runs if RAM exists.
    psramChipBytes = psramFound() ? esp_spiram_get_size() : 0;
    psramTestOk = psramFound() && testPsram();
    delay(1500);
    printReport();
    lastReport = millis();
}

void loop() {
    if (millis() - lastReport >= 2000) {
        lastReport = millis();
        printReport();
    }
    delay(10);
}
