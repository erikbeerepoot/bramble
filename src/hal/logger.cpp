#include "logger.h"

#include <cstdio>
#include <cstring>

#include "../storage/log_flash_buffer.h"

// Initialize static members
LogLevel Logger::global_level_ = LogLevel::Info;
bool Logger::check_usb_ = false;
uint64_t Logger::rtc_sync_us_ = 0;
LogFlashBuffer *Logger::flash_sink_ = nullptr;
LogLevel Logger::flash_level_ = LogLevel::Info;  // Default: log Info and above to flash

void Logger::writeToFlash(LogLevel level, const char *fmt, va_list args) const
{
    char msg_buf[108];
    vsnprintf(msg_buf, sizeof(msg_buf), fmt, args);
    flash_sink_->writeLog(static_cast<uint8_t>(level), module_name_, msg_buf, getTimestampMs());
}
