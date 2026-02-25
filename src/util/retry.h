#pragma once

#include "../hal/logger.h"
#include "pico/stdlib.h"

/**
 * @brief Retry an action up to maxAttempts times with a fixed delay between attempts.
 *
 * @tparam Func Callable returning bool (true = success, false = retry)
 * @param maxAttempts Maximum number of attempts (must be >= 1)
 * @param delayUs Microseconds to wait between retries (0 = no delay)
 * @param description Human-readable name for log messages
 * @param logger Logger instance for warnings/errors
 * @param action Callable that returns true on success
 * @return true if action succeeded within maxAttempts, false otherwise
 */
template <typename Func>
bool retryWithBackoff(int maxAttempts, uint32_t delayUs, const char *description, Logger &logger,
                      Func action)
{
    for (int attempt = 0; attempt < maxAttempts; attempt++) {
        if (attempt > 0) {
            logger.warn("%s: retry %d/%d", description, attempt + 1, maxAttempts);
            if (delayUs > 0) {
                sleep_us(delayUs);
            }
        }
        if (action()) {
            return true;
        }
    }
    logger.error("%s: failed after %d attempts", description, maxAttempts);
    return false;
}
