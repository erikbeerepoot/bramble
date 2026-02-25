#pragma once

#include "pico/stdlib.h"

#include "../hal/logger.h"

/**
 * @brief Retry an action up to maxAttempts times with exponential backoff.
 *
 * The first retry waits delayUs, the second waits 2*delayUs, the third 4*delayUs, etc.
 * Pass delayUs=0 for no delay between attempts.
 *
 * @tparam Func Callable returning bool (true = success, false = retry)
 * @param maxAttempts Maximum number of attempts (must be >= 1)
 * @param delayUs Base delay in microseconds for the first retry; doubles on each subsequent retry
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
                // Exponential backoff: delayUs on first retry, 2x on second, 4x on third, ...
                sleep_us(delayUs << (uint32_t)(attempt - 1));
            }
        }
        if (action()) {
            return true;
        }
    }
    logger.error("%s: failed after %d attempts", description, maxAttempts);
    return false;
}
