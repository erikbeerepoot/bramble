#pragma once

#include <cstdint>
#include <cstdio>

/**
 * @brief Safe uint64_t formatting for RP2040
 *
 * The Pico SDK's printf and newlib-nano's snprintf cannot handle %llu/%llX
 * on 32-bit ARM — they misparse the 64-bit value from varargs, corrupting
 * subsequent arguments and potentially the stack.
 *
 * Use these helpers with %s instead of %llu/%llX.
 */
namespace bramble {
namespace format {

/**
 * @brief Convert uint64_t to decimal string
 * @param value The value to convert
 * @param buf Output buffer (must be at least 21 bytes for max uint64)
 * @param buf_size Size of output buffer
 */
inline void uint64_to_str(uint64_t value, char *buf, size_t buf_size)
{
    if (buf_size == 0) return;
    if (value == 0) {
        snprintf(buf, buf_size, "0");
        return;
    }
    char tmp[21];  // max uint64 is 20 digits + NUL
    int pos = sizeof(tmp) - 1;
    tmp[pos] = '\0';
    while (value > 0 && pos > 0) {
        tmp[--pos] = '0' + (value % 10);
        value /= 10;
    }
    snprintf(buf, buf_size, "%s", &tmp[pos]);
}

/**
 * @brief Convert uint64_t to zero-padded hex string (16 chars)
 * @param value The value to convert
 * @param buf Output buffer (must be at least 17 bytes)
 * @param buf_size Size of output buffer
 */
inline void uint64_to_hex(uint64_t value, char *buf, size_t buf_size)
{
    if (buf_size == 0) return;
    uint32_t hi = static_cast<uint32_t>(value >> 32);
    uint32_t lo = static_cast<uint32_t>(value);
    snprintf(buf, buf_size, "%08lX%08lX", hi, lo);
}

}  // namespace format
}  // namespace bramble
