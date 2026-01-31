#include "logger.h"

// Initialize static members
LogLevel Logger::global_level_ = LogLevel::Info;  // Default to INFO level
bool Logger::check_usb_ = false;                  // Default: always log (for development)
uint64_t Logger::rtc_sync_us_ = 0;                // Not synced until rtc_set_datetime called