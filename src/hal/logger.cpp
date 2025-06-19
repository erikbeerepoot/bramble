#include "logger.h"
#include "pico/stdlib.h"
#include <cstring>

// Initialize static members
LogLevel Logger::global_level_ = LOG_INFO;  // Default to INFO level
bool Logger::usb_check_enabled_ = false;    // Disabled by default for simplicity
bool Logger::usb_connected_ = true;         // Assume connected by default

Logger::Logger(const char* module_name) : module_name_(module_name) {
}

void Logger::error(const char* fmt, ...) const {
    if (!isLoggingEnabled(LOG_ERROR)) return;
    
    va_list args;
    va_start(args, fmt);
    logInternal(LOG_ERROR, "ERR", fmt, args);
    va_end(args);
}

void Logger::warn(const char* fmt, ...) const {
    if (!isLoggingEnabled(LOG_WARN)) return;
    
    va_list args;
    va_start(args, fmt);
    logInternal(LOG_WARN, "WARN", fmt, args);
    va_end(args);
}

void Logger::info(const char* fmt, ...) const {
    if (!isLoggingEnabled(LOG_INFO)) return;
    
    va_list args;
    va_start(args, fmt);
    logInternal(LOG_INFO, "INFO", fmt, args);
    va_end(args);
}

void Logger::debug(const char* fmt, ...) const {
    if (!isLoggingEnabled(LOG_DEBUG)) return;
    
    va_list args;
    va_start(args, fmt);
    logInternal(LOG_DEBUG, "DBG", fmt, args);
    va_end(args);
}

void Logger::logInternal(LogLevel level, const char* prefix, const char* fmt, va_list args) const {
    // Format: "LEVEL [MODULE]: message"
    printf("%s [%s]: ", prefix, module_name_);
    vprintf(fmt, args);
    printf("\n");
}

void Logger::setLogLevel(LogLevel level) {
    global_level_ = level;
}

LogLevel Logger::getLogLevel() {
    return global_level_;
}

const char* Logger::logLevelToString(LogLevel level) {
    switch (level) {
        case LOG_NONE:  return "NONE";
        case LOG_ERROR: return "ERROR";
        case LOG_WARN:  return "WARN";
        case LOG_INFO:  return "INFO";
        case LOG_DEBUG: return "DEBUG";
        default:        return "UNKNOWN";
    }
}

void Logger::setUsbCheckEnabled(bool enabled) {
    usb_check_enabled_ = enabled;
    if (enabled) {
        updateUsbStatus();  // Update status immediately when enabled
    }
}

void Logger::updateUsbStatus() {
    if (!usb_check_enabled_) {
        usb_connected_ = true;  // Always log when USB check is disabled
        return;
    }
    
    // Simple heuristic: Check if stdio USB is initialized
    // In a real implementation, you might check USB enumeration status
    //TODO : Replace with actual USB connection check
    usb_connected_ = stdio_usb_connected();
}

bool Logger::isLoggingEnabled(LogLevel level) {
    // Check global log level first
    if (global_level_ < level) {
        return false;
    }
    
    // Check USB connection if enabled
    if (usb_check_enabled_ && !usb_connected_) {
        return false;
    }
    
    return true;
}