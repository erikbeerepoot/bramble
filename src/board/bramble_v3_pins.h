#pragma once

/**
 * @file bramble_v3_pins.h
 * @brief Pin definitions for Bramble v3 board (Adafruit Feather RP2040 RFM9x)
 *
 * LoRa and external flash share SPI1.
 */

#include "hardware/i2c.h"
#include "hardware/spi.h"
#include "hardware/uart.h"

namespace Board {

// --- LoRa (SX1276 on SPI1, shared with external flash) ---
inline auto LORA_SPI_PORT = spi1;
constexpr uint LORA_PIN_MISO = 8;
constexpr uint LORA_PIN_SCK = 14;
constexpr uint LORA_PIN_MOSI = 15;
constexpr uint LORA_PIN_CS = 16;
constexpr uint LORA_PIN_RST = 17;
constexpr uint LORA_PIN_DIO0 = 21;

// --- External flash (shares SPI1 with LoRa) ---
constexpr bool FLASH_HAS_DEDICATED_SPI = false;
inline auto FLASH_SPI_PORT = spi1;   // Same as LoRa
constexpr uint FLASH_PIN_MISO = 8;   // Shared
constexpr uint FLASH_PIN_SCK = 14;   // Shared
constexpr uint FLASH_PIN_MOSI = 15;  // Shared
constexpr uint FLASH_PIN_CS = 6;
constexpr uint FLASH_PIN_RST = 7;

// --- QSPI flash size (on-board) ---
constexpr uint32_t QSPI_FLASH_SIZE = 8 * 1024 * 1024;  // 8MB

// --- NeoPixel ---
constexpr uint PIN_NEOPIXEL = 4;

// --- I2C (sensors) ---
inline auto SENSOR_I2C_PORT = i2c1;
constexpr uint PIN_I2C_SDA = 26;
constexpr uint PIN_I2C_SCL = 27;

// --- Analog/digital inputs ---
constexpr uint PIN_A0 = 26;
constexpr uint PIN_A1 = 27;

// --- PMU UART (UART0) ---
inline auto PMU_UART_PORT = uart0;
constexpr uint PMU_UART_TX_PIN = 0;
constexpr uint PMU_UART_RX_PIN = 1;

// --- API/Debug UART (UART1) ---
inline auto API_UART_PORT = uart1;
constexpr uint API_UART_TX_PIN = 24;
constexpr uint API_UART_RX_PIN = 25;

// --- Default stdio UART ---
constexpr int DEFAULT_UART = 1;
constexpr int DEFAULT_UART_TX_PIN = 24;
constexpr int DEFAULT_UART_RX_PIN = -1;

// --- Valve / Motor pins ---
constexpr uint8_t NUM_VALVES = 2;
constexpr uint8_t PIN_MOTOR_LO_1 = 26;
constexpr uint8_t PIN_MOTOR_LO_2 = 27;
constexpr uint8_t PIN_MOTOR_HI_1 = 28;
constexpr uint8_t PIN_MOTOR_HI_2 = 29;
constexpr uint8_t VALVE_PINS[NUM_VALVES] = {24, 25};

// --- Curtain motor pins (greenhouse variant) ---
// Relay-based motor reversing: one GPIO per direction
constexpr uint8_t PIN_CURTAIN_OPEN = 28;
constexpr uint8_t PIN_CURTAIN_CLOSE = 29;

}  // namespace Board
