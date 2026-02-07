/**
 * @file main.h
 * @brief Common functions used across the application
 */

#ifndef MAIN_H
#define MAIN_H

#include <cstdint>

/**
 * @brief Get the unique device ID from the Pico's flash
 *
 * Reads the 8-byte unique board ID and converts it to a 64-bit value
 * using big-endian byte order (MSB first).
 *
 * @return uint64_t The device ID
 */
uint64_t getDeviceId();

#endif // MAIN_H
