#pragma once

/**
 * @brief Initialize composite USB CDC+MSC and register the CDC stdio driver
 *
 * Replaces pico_stdio_usb with a custom driver that supports both
 * CDC (serial console) and MSC (log mass storage) simultaneously.
 * Call after stdio_init_all() to add USB CDC as an additional stdio output.
 *
 * @return true on success
 */
bool usb_stdio_init(void);

/**
 * @brief Check if USB CDC is connected (host has opened the serial port)
 * @return true if USB serial is connected
 */
bool usb_stdio_connected(void);
