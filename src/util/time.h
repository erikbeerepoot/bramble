#pragma once

#include <cstdint>
#include <ctime>

#include "hardware/rtc.h"

#include "../hal/pmu_protocol.h"

namespace bramble::util::time {

inline uint32_t uptimeSeconds(uint32_t bootTimestamp, uint32_t currentTimestamp)
{
    if (bootTimestamp > 0 && currentTimestamp > bootTimestamp) {
        return currentTimestamp - bootTimestamp;
    }
    return 0;
}

/// Convert PMU::DateTime (2-digit year) to RP2040 RTC datetime_t
inline datetime_t toDatetimeT(const PMU::DateTime &dt)
{
    datetime_t result;
    result.year = 2000 + dt.year;
    result.month = dt.month;
    result.day = dt.day;
    result.dotw = dt.weekday;
    result.hour = dt.hour;
    result.min = dt.minute;
    result.sec = dt.second;
    return result;
}

/// Convert PMU::DateTime (2-digit year) to Unix timestamp (seconds since epoch)
inline uint32_t toUnixTimestamp(const PMU::DateTime &dt)
{
    std::tm tm = {};
    tm.tm_year = (2000 + dt.year) - 1900;
    tm.tm_mon = dt.month - 1;
    tm.tm_mday = dt.day;
    tm.tm_hour = dt.hour;
    tm.tm_min = dt.minute;
    tm.tm_sec = dt.second;
    return static_cast<uint32_t>(std::mktime(&tm));
}

}  // namespace bramble::util::time