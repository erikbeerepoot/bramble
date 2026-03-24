/**
 * @file bramble_v4.h
 * @brief Pico SDK board definition for Bramble v4 (custom RP2350B board)
 *
 * This file is referenced by the SDK via PICO_BOARD and PICO_BOARD_HEADER_DIRS.
 */

#ifndef _BOARDS_BRAMBLE_V4_H
#define _BOARDS_BRAMBLE_V4_H

// --- Chip variant ---
// RP2350B has 48 GPIOs (vs 30 on standard RP2350A)
#ifndef PICO_RP2350A
#define PICO_RP2350A 0
#endif

// --- Flash ---
// 16MB QSPI flash (W25Q128JV or similar)
#ifndef PICO_FLASH_SPI_CLKDIV
#define PICO_FLASH_SPI_CLKDIV 2
#endif

#ifndef PICO_FLASH_SIZE_BYTES
#define PICO_FLASH_SIZE_BYTES (16 * 1024 * 1024)
#endif

// --- Crystal ---
#ifndef PICO_XOSC_STARTUP_DELAY_MULTIPLIER
#define PICO_XOSC_STARTUP_DELAY_MULTIPLIER 64
#endif

// --- UART defaults ---
// Default UART0 on GPIO28/29 for debug probe (stdio)
// PMU uses UART1 on GPIO8/9 (configured in firmware)
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
// No on-board LED (NeoPixel is used instead, configured in firmware)
#ifndef PICO_DEFAULT_LED_PIN
#define PICO_DEFAULT_LED_PIN 4
#endif

#ifndef PICO_DEFAULT_WS2812_PIN
#define PICO_DEFAULT_WS2812_PIN 4
#endif

// --- Board identification ---
#define BRAMBLE_V4_BOARD 1

#endif // _BOARDS_BRAMBLE_V4_H
