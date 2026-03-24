#include "rtc_compat.h"

#if PICO_RP2040

// RP2040: thin wrappers around hardware_rtc

void bramble_rtc_init()
{
    rtc_init();
}

bool bramble_rtc_set_datetime(const datetime_t *dt)
{
    // hardware_rtc takes non-const pointer but doesn't modify
    return rtc_set_datetime(const_cast<datetime_t *>(dt));
}

bool bramble_rtc_get_datetime(datetime_t *dt)
{
    return rtc_get_datetime(dt);
}

bool bramble_rtc_running()
{
    return rtc_running();
}

#else

// RP2350: use pico_aon_timer with struct tm conversion

#include <ctime>

#include "pico/aon_timer.h"

static bool aon_started = false;

static struct tm datetime_to_tm(const datetime_t *dt)
{
    struct tm tm = {};
    tm.tm_year = dt->year - 1900;
    tm.tm_mon  = dt->month - 1;
    tm.tm_mday = dt->day;
    tm.tm_wday = dt->dotw;
    tm.tm_hour = dt->hour;
    tm.tm_min  = dt->min;
    tm.tm_sec  = dt->sec;
    return tm;
}

static void tm_to_datetime(const struct tm *tm, datetime_t *dt)
{
    dt->year  = static_cast<int16_t>(tm->tm_year + 1900);
    dt->month = static_cast<int8_t>(tm->tm_mon + 1);
    dt->day   = static_cast<int8_t>(tm->tm_mday);
    dt->dotw  = static_cast<int8_t>(tm->tm_wday);
    dt->hour  = static_cast<int8_t>(tm->tm_hour);
    dt->min   = static_cast<int8_t>(tm->tm_min);
    dt->sec   = static_cast<int8_t>(tm->tm_sec);
}

void bramble_rtc_init()
{
    // AON timer is started on first set_datetime call
}

bool bramble_rtc_set_datetime(const datetime_t *dt)
{
    struct tm tm = datetime_to_tm(dt);

    if (!aon_started) {
        aon_timer_start_calendar(&tm);
        aon_started = true;
    } else {
        aon_timer_set_time_calendar(&tm);
    }
    return true;
}

bool bramble_rtc_get_datetime(datetime_t *dt)
{
    if (!aon_started) {
        return false;
    }

    struct tm tm = {};
    if (!aon_timer_get_time_calendar(&tm)) {
        return false;
    }

    tm_to_datetime(&tm, dt);
    return true;
}

bool bramble_rtc_running()
{
    return aon_started && aon_timer_is_running();
}

#endif
