#pragma once

/**
 * @file rtc_compat.h
 * @brief RTC compatibility layer for RP2040 and RP2350
 *
 * RP2040 has a hardware RTC (hardware_rtc) with datetime_t.
 * RP2350 has no hardware RTC; we use pico_aon_timer (always-on timer)
 * which works with struct timespec / struct tm internally.
 *
 * This layer provides a unified interface. On RP2040 datetime_t comes
 * from hardware/rtc.h. On RP2350 we define a compatible struct.
 */

#include <cstdint>

// On RP2040, datetime_t is provided by hardware/rtc.h.
// On RP2350, it doesn't exist so we define it ourselves.
#if PICO_RP2040
#include "hardware/rtc.h"
#else
// Minimal datetime_t definition matching the RP2040 SDK struct
typedef struct {
    int16_t year;  ///< 0..4095
    int8_t month;  ///< 1..12
    int8_t day;    ///< 1..28,29,30,31
    int8_t dotw;   ///< 0..6, 0 is Sunday
    int8_t hour;   ///< 0..23
    int8_t min;    ///< 0..59
    int8_t sec;    ///< 0..59
} datetime_t;
#endif

/**
 * @brief Initialize the RTC/AON timer
 */
void bramble_rtc_init();

/**
 * @brief Set the current date and time
 * @param dt Pointer to datetime_t structure
 * @return true if successful
 */
bool bramble_rtc_set_datetime(const datetime_t *dt);

/**
 * @brief Get the current date and time
 * @param dt Pointer to datetime_t structure to fill
 * @return true if successful
 */
bool bramble_rtc_get_datetime(datetime_t *dt);

/**
 * @brief Check if the RTC/timer is running with a valid time
 * @return true if running
 */
bool bramble_rtc_running();

// On RP2350, provide SDK-compatible function names so existing code that calls
// rtc_init(), rtc_set_datetime(), rtc_get_datetime(), rtc_running() compiles
// without changes. On RP2040 these are already provided by hardware/rtc.h.
#if !PICO_RP2040
inline void rtc_init()
{
    bramble_rtc_init();
}
inline bool rtc_set_datetime(datetime_t *dt)
{
    return bramble_rtc_set_datetime(dt);
}
inline bool rtc_get_datetime(datetime_t *dt)
{
    return bramble_rtc_get_datetime(dt);
}
inline bool rtc_running()
{
    return bramble_rtc_running();
}
#endif
