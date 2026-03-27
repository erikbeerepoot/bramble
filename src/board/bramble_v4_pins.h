#pragma once

/**
 * @file bramble_v4_pins.h
 * @brief Pin definitions for Bramble v4 board (custom RP2350B)
 *
 * Key difference from v3: LoRa uses SPI1, external flash uses dedicated SPI0.
 * PMU on UART1 (GPIO8/9), debug/stdio on UART1 (GPIO24/25).
 * Supports up to 4 valves.
 */

#include "hardware/i2c.h"
#include "hardware/spi.h"
#include "hardware/uart.h"

namespace Board {

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
constexpr uint32_t QSPI_FLASH_SIZE = 16 * 1024 * 1024;  // 16MB

// --- NeoPixel ---
constexpr uint PIN_NEOPIXEL = 4;

// --- I2C (sensors) ---
inline auto SENSOR_I2C_PORT = i2c1;
constexpr uint PIN_I2C_SDA = 26;
constexpr uint PIN_I2C_SCL = 27;

// --- Analog/digital inputs ---
constexpr uint PIN_A0 = 26;
constexpr uint PIN_A1 = 27;

// --- PMU UART (UART1 on GPIO8/9) ---
inline auto PMU_UART_PORT = uart1;
constexpr uint PMU_UART_TX_PIN = 8;
constexpr uint PMU_UART_RX_PIN = 9;

// --- Default stdio UART (UART0 on GPIO28/29 — debug probe) ---
constexpr int DEFAULT_UART = 0;
constexpr int DEFAULT_UART_TX_PIN = 28;
constexpr int DEFAULT_UART_RX_PIN = 29;

// --- Valve / Motor pins (4 valves on v4) ---
constexpr uint8_t NUM_VALVES = 4;
constexpr uint8_t PIN_MOTOR_LO_1 = 28;
constexpr uint8_t PIN_MOTOR_LO_2 = 29;
constexpr uint8_t PIN_MOTOR_HI_1 = 30;
constexpr uint8_t PIN_MOTOR_HI_2 = 31;
constexpr uint8_t VALVE_PINS[NUM_VALVES] = {32, 33, 34, 35};

// --- Curtain motor pins (greenhouse variant) ---
// Relay-based motor reversing: one GPIO per direction
constexpr uint8_t PIN_CURTAIN_OPEN = 30;
constexpr uint8_t PIN_CURTAIN_CLOSE = 31;

}  // namespace Board
