#pragma once

/**
 * @file bramble_v4_pins.h
 * @brief Pin definitions for Bramble v4 board (custom RP2350B)
 *
 * Physical headers define the GPIO-to-connector mapping. Functional constants
 * reference header pins so the relationship between schematic, PCB, and code
 * is explicit.
 *
 * NOTE: J7 PCB silkscreen labels use v3-era numbering (GPIO28-31) but the
 *       actual RP2350B GPIOs are 36-39. The namespaces below use the real
 *       GPIO numbers.
 */

#include "hardware/i2c.h"
#include "hardware/spi.h"
#include "hardware/uart.h"

namespace Board {

// ==========================================================================
// Physical header definitions (GPIO numbers for each connector pin)
// Pin 1 = +3V3, Pin 2 = +9V, Pin 11 = VDD, Pin 12 = GND on all headers
// ==========================================================================

namespace J6 {                  // Motor/Valve connector (GPIO 28-35)
constexpr uint8_t PIN_3 = 35;   // VALVE_3
constexpr uint8_t PIN_4 = 34;   // MOTOR_LO_2
constexpr uint8_t PIN_5 = 33;   // VALVE_4
constexpr uint8_t PIN_6 = 32;   // MOTOR_LO_1
constexpr uint8_t PIN_7 = 31;   // VALVE_2
constexpr uint8_t PIN_8 = 30;   // MOTOR_HI_2
constexpr uint8_t PIN_9 = 29;   // VALVE_1
constexpr uint8_t PIN_10 = 28;  // MOTOR_HI_1
}  // namespace J6

namespace J7 {  // GPIO/debug header (GPIO 36-43)
// PCB silkscreen uses v3 numbering — real RP2350B GPIOs listed here
constexpr uint8_t PIN_3 = 43;   // labeled GPIO43_ADC3
constexpr uint8_t PIN_4 = 42;   // labeled GPIO42_ADC2
constexpr uint8_t PIN_5 = 41;   // labeled GPIO41_ADC1
constexpr uint8_t PIN_6 = 40;   // labeled GPIO40_ADC0
constexpr uint8_t PIN_7 = 39;   // labeled GPIO31
constexpr uint8_t PIN_8 = 38;   // labeled GPIO30
constexpr uint8_t PIN_9 = 37;   // labeled GPIO29
constexpr uint8_t PIN_10 = 36;  // labeled GPIO28
}  // namespace J7

namespace J8 {  // General purpose header (GPIO 20-27)
constexpr uint8_t PIN_3 = 26;
constexpr uint8_t PIN_4 = 27;
constexpr uint8_t PIN_5 = 24;
constexpr uint8_t PIN_6 = 25;
constexpr uint8_t PIN_7 = 22;
constexpr uint8_t PIN_8 = 23;
constexpr uint8_t PIN_9 = 20;
constexpr uint8_t PIN_10 = 21;
}  // namespace J8

// ==========================================================================
// Functional pin assignments (reference header pins)
// ==========================================================================

// --- LoRa (SX1262 / NiceRF LORA1262 on SPI1) ---
// NOTE: MOSI/SCK swapped vs RP2350 pin mux — needs bit-bang SPI
// DIO1 and BUSY need bodge wire to RFM_IO pads
inline auto LORA_SPI_PORT = spi1;
constexpr uint LORA_PIN_MISO = 12;
constexpr uint LORA_PIN_MOSI = 14;  // PCB trace to SX1262 MOSI (RP2350 pin mux = SPI1 SCK)
constexpr uint LORA_PIN_SCK = 15;   // PCB trace to SX1262 SCK  (RP2350 pin mux = SPI1 TX)
constexpr uint LORA_PIN_CS = 10;
constexpr uint LORA_PIN_RST = 16;
constexpr uint LORA_PIN_DIO1 = 19;  // DIO1 bodge wire → GPIO19
constexpr uint LORA_PIN_BUSY = 17;  // BUSY bodge wire → GPIO17

// --- External flash (dedicated SPI0) ---
constexpr bool FLASH_HAS_DEDICATED_SPI = true;
inline auto FLASH_SPI_PORT = spi0;
constexpr uint FLASH_PIN_MISO = 0;
constexpr uint FLASH_PIN_SCK = 2;
constexpr uint FLASH_PIN_MOSI = 3;
constexpr uint FLASH_PIN_CS = 1;
constexpr uint FLASH_PIN_RST = 5;

// --- QSPI flash size (on-board) ---
constexpr uint32_t QSPI_FLASH_SIZE = 2 * 1024 * 1024;  // 2MB (W25Q16JV)

// --- NeoPixel ---
constexpr uint PIN_NEOPIXEL = 4;

// --- I2C (sensors) --- (J8 pins 3/4)
inline auto SENSOR_I2C_PORT = i2c1;
constexpr uint PIN_I2C_SDA = J8::PIN_3;  // GPIO 26
constexpr uint PIN_I2C_SCL = J8::PIN_4;  // GPIO 27

// --- Analog/digital inputs ---
constexpr uint PIN_A0 = J8::PIN_3;  // GPIO 26
constexpr uint PIN_A1 = J8::PIN_4;  // GPIO 27

// --- PMU UART (UART1 on GPIO8/9) ---
inline auto PMU_UART_PORT = uart1;
constexpr uint PMU_UART_TX_PIN = 8;
constexpr uint PMU_UART_RX_PIN = 9;

// --- Default stdio UART (UART0 on J6 header — no conflict with PMU on UART1) ---
constexpr int DEFAULT_UART = 0;
constexpr int DEFAULT_UART_TX_PIN = J6::PIN_10;  // GPIO 28 (UART0 TX)
constexpr int DEFAULT_UART_RX_PIN = J6::PIN_9;   // GPIO 29 (UART0 RX)

// --- Valve / Motor pins (4 valves on v4, J7 header) ---
// Valve connector plugs into J7 instead of J6 to free J6 for UART0
constexpr uint8_t NUM_VALVES = 4;
constexpr uint8_t PIN_MOTOR_HI_1 = J7::PIN_10;  // GPIO 36
constexpr uint8_t PIN_MOTOR_HI_2 = J7::PIN_8;   // GPIO 38
constexpr uint8_t PIN_MOTOR_LO_1 = J7::PIN_6;   // GPIO 40
constexpr uint8_t PIN_MOTOR_LO_2 = J7::PIN_4;   // GPIO 42
constexpr uint8_t VALVE_PINS[NUM_VALVES] = {
    J7::PIN_9,  // VALVE_1 = GPIO 37
    J7::PIN_7,  // VALVE_2 = GPIO 39
    J7::PIN_3,  // VALVE_3 = GPIO 43
    J7::PIN_5,  // VALVE_4 = GPIO 41
};

// --- Curtain motor pins (greenhouse variant, J8 header) ---
// Relay-based motor reversing: one GPIO per direction
constexpr uint8_t PIN_CURTAIN_OPEN = J8::PIN_7;   // GPIO 22
constexpr uint8_t PIN_CURTAIN_CLOSE = J8::PIN_8;  // GPIO 23

}  // namespace Board
