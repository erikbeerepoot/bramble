#include "logger.h"

// Initialize static members
LogLevel Logger::global_level_ = LogLevel::Info;
uint64_t Logger::rtc_sync_us_ = 0;
