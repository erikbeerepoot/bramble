#pragma once

#include "pico/stdlib.h"

/**
 * @brief Common time utilities for the codebase
 */
namespace TimeUtils {
/**
 * @brief Get current time in milliseconds since boot
 * @return Current time in milliseconds
 */
inline uint32_t getCurrentTimeMs()
{
    return to_ms_since_boot(get_absolute_time());
}

/**
 * @brief Get current time in seconds since boot
 * @return Current time in seconds
 */
inline uint32_t getCurrentTimeSec()
{
    return getCurrentTimeMs() / 1000;
}

/**
 * @brief Check if a timeout has elapsed
 * @param start_time Start time in milliseconds
 * @param timeout_ms Timeout duration in milliseconds
 * @return true if timeout has elapsed
 */
inline bool hasTimedOut(uint32_t start_time, uint32_t timeout_ms)
{
    return (getCurrentTimeMs() - start_time) >= timeout_ms;
}

/**
 * @brief Calculate time elapsed since start
 * @param start_time Start time in milliseconds
 * @return Elapsed time in milliseconds
 */
inline uint32_t getElapsedMs(uint32_t start_time)
{
    return getCurrentTimeMs() - start_time;
}
}  // namespace TimeUtils