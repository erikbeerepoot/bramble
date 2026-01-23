#include "logger.h"

// Initialize static members
LogLevel Logger::global_level_ = LogLevel::Info;  // Default to INFO level
bool Logger::check_usb_ = false;                  // Default: always log (for development)