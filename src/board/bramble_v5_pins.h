#pragma once

/**
 * @file bramble_v5_pins.h
 * @brief Pin definitions for Bramble v5 board (custom RP2350B)
 *
 * v5 fixes the v4 PCB wiring defects:
 *   - LoRa MOSI/SCK no longer swapped — hardware SPI1 works directly
 *   - SX1262 DIO1 / BUSY routed natively (no bodge wires)
 *   - PMU UART RX on GPIO9 functional (no bodge to GPIO21)
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
constexpr uint8_t PIN_3 = 43;
constexpr uint8_t PIN_4 = 42;
constexpr uint8_t PIN_5 = 41;
constexpr uint8_t PIN_6 = 40;
constexpr uint8_t PIN_7 = 39;
constexpr uint8_t PIN_8 = 38;
constexpr uint8_t PIN_9 = 37;
constexpr uint8_t PIN_10 = 36;
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
// Functional pin assignments
// ==========================================================================

// --- LoRa (SX1262 / NiceRF LORA1262 on hardware SPI1) ---
// v5: PCB routes MOSI to GPIO15 and SCK to GPIO14 — matches SPI1 pin mux,
// so hardware SPI works directly (no bit-bang needed unlike v4).
inline auto LORA_SPI_PORT = spi1;
constexpr uint LORA_PIN_MISO = 12;  // SPI1 RX
constexpr uint LORA_PIN_SCK = 14;   // SPI1 SCK
constexpr uint LORA_PIN_MOSI = 15;  // SPI1 TX
constexpr uint LORA_PIN_CS = 10;    // GPIO chip-select (not hardware CSn)
constexpr uint LORA_PIN_RST = 16;
constexpr uint LORA_PIN_DIO1 = 19;
constexpr uint LORA_PIN_BUSY = 18;

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

// --- API UART (hub ↔ Raspberry Pi / CM5) — UART0 on J6 (stdio is on USB CDC) ---
inline auto API_UART_PORT = uart0;
constexpr uint API_UART_TX_PIN = J6::PIN_10;  // GPIO 28
constexpr uint API_UART_RX_PIN = J6::PIN_9;   // GPIO 29

// --- Valve / Motor pins (4 valves, J7 header) ---
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

// --- Curtain motor pins (greenhouse variant, J7 header) ---
constexpr uint8_t PIN_CURTAIN_OPEN = J7::PIN_6;   // GPIO 40
constexpr uint8_t PIN_CURTAIN_CLOSE = J7::PIN_8;  // GPIO 38

}  // namespace Board
