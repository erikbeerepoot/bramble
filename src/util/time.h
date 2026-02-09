#pragma once

#include <cstdint>

namespace bramble::util::time {

inline uint32_t uptimeSeconds(uint32_t bootTimestamp, uint32_t currentTimestamp)
{
    if (bootTimestamp > 0 && currentTimestamp > bootTimestamp) {
        return currentTimestamp - bootTimestamp;
    }
    return 0;
}

}  // namespace bramble::util::time