/**
 * @file bramble_v5.h
 * @brief Pico SDK board definition for Bramble v5 (custom RP2350B board)
 *
 * v5 fixes v4 PCB defects: native LoRa SPI mux, native SX1262 DIO1/BUSY,
 * native UART1 RX on GPIO9. Same chip + flash + headers as v4.
 *
 * Referenced by the SDK via PICO_BOARD and PICO_BOARD_HEADER_DIRS.
 */

#ifndef _BOARDS_BRAMBLE_V5_H
#define _BOARDS_BRAMBLE_V5_H

// --- Chip variant ---
// RP2350B has 48 GPIOs (vs 30 on standard RP2350A)
#ifndef PICO_RP2350A
#define PICO_RP2350A 0
#endif

// --- Flash ---
#ifndef PICO_FLASH_SPI_CLKDIV
#define PICO_FLASH_SPI_CLKDIV 2
#endif

#ifndef PICO_FLASH_SIZE_BYTES
#define PICO_FLASH_SIZE_BYTES (2 * 1024 * 1024)
#endif

// --- Crystal ---
#ifndef PICO_XOSC_STARTUP_DELAY_MULTIPLIER
#define PICO_XOSC_STARTUP_DELAY_MULTIPLIER 64
#endif

// --- UART defaults ---
// Stdio on UART0 GPIO28/29 (J6 header pins 10/9)
// PMU uses UART1 on GPIO8/9 — no peripheral conflict
#ifndef PICO_DEFAULT_UART
#define PICO_DEFAULT_UART 0
#endif

#ifndef PICO_DEFAULT_UART_TX_PIN
#define PICO_DEFAULT_UART_TX_PIN 28
#endif

#ifndef PICO_DEFAULT_UART_RX_PIN
#define PICO_DEFAULT_UART_RX_PIN 29
#endif

// --- LED ---
#ifndef PICO_DEFAULT_LED_PIN
#define PICO_DEFAULT_LED_PIN 4
#endif

#ifndef PICO_DEFAULT_WS2812_PIN
#define PICO_DEFAULT_WS2812_PIN 4
#endif

// --- Board identification ---
#define BRAMBLE_V5_BOARD 1

#endif  // _BOARDS_BRAMBLE_V5_H
