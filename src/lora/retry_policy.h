#pragma once

#include <cstdint>
#include "message.h"

/**
 * @brief Policy-based retry system with different strategies per criticality
 */
class RetryPolicy {
public:
    struct RetryConfig {
        uint32_t base_delay_ms;
        uint32_t max_delay_ms;
        uint8_t max_attempts;
        bool exponential_backoff;
        bool infinite_retry;
    };
    
    /**
     * @brief Get retry configuration for given criticality level
     */
    static RetryConfig getConfig(DeliveryCriticality criticality) {
        switch (criticality) {
            case BEST_EFFORT:
                return { 0, 0, 0, false, false };  // No retries
                
            case RELIABLE:
                return { 1000, 5000, 3, true, false };  // 3 attempts with backoff
                
            case CRITICAL:
                return { 2000, 30000, 10, true, true };  // Many attempts, then infinite
                
            default:
                return { 0, 0, 0, false, false };
        }
    }
    
    /**
     * @brief Calculate next retry delay
     */
    static uint32_t calculateDelay(const RetryConfig& config, uint8_t attempt) {
        if (attempt == 0 || config.base_delay_ms == 0) return config.base_delay_ms;
        
        uint32_t delay = config.base_delay_ms;
        
        if (config.exponential_backoff) {
            // Exponential backoff: delay * 2^(attempt-1)
            delay = delay << (attempt - 1);
        }
        
        // Cap at maximum delay
        if (delay > config.max_delay_ms) {
            delay = config.max_delay_ms;
        }
        
        return delay;
    }
    
    /**
     * @brief Check if should continue retrying
     */
    static bool shouldRetry(const RetryConfig& config, uint8_t attempt) {
        if (config.infinite_retry && attempt >= config.max_attempts) {
            return true;  // Continue with max delay
        }
        return attempt < config.max_attempts;
    }
    
    /**
     * @brief Get human-readable description of policy
     */
    static const char* getPolicyName(DeliveryCriticality criticality) {
        switch (criticality) {
            case BEST_EFFORT: return "Fire-and-forget";
            case RELIABLE:    return "Reliable (3 retries)";
            case CRITICAL:    return "Critical (persistent)";
            default:          return "Unknown";
        }
    }
};