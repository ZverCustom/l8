#pragma once
// L8 PCB v3: PCB/v3/gen/pinmap.py is the hardware source of truth.
#define BOOT_BUTTON_PIN 0
#define VBAT_SENSE_PIN 1
#define LED_ISENSE_PIN 2
#define LED_PIN_1 11
#define LED_PIN_2 12
#define LED_PIN_3 13
#define LED_PIN_4 14
#define LED_PIN_5 15
#define LED_PIN_6 16
#define LED_PIN_7 17
#define LED_PIN_8 18
#define IMU_SDA_PIN 8
#define IMU_SCL_PIN 9
#define IMU_INT1_PIN 42
#define K_RX_PIN 36
#define K_TX_PIN 35
#define CAN_RX_PIN 39
#define CAN_TX_PIN 38
#define CAN_STB_PIN -1  // PCA9555 P15, never an ESP GPIO
#define IGN_PIN 40
#define HOLD_PIN 41
#define SENSOR_FAULT_PIN 47
constexpr int INPUT_SENSE_PINS[3] = {4,5,6};
constexpr int INPUT_PULL_PINS[3] = {7,10,21};
