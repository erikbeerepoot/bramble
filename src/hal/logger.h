#pragma once
#include <cstdio>
#include <cstdarg>
#include "pico/stdio_usb.h"

/**
 * @brief Logging levels for debug output control
 */
enum LogLevel {
    LOG_NONE = 0,    // No logging
    LOG_ERROR = 1,   // Critical errors only
    LOG_WARN = 2,    // Warnings and errors
    LOG_INFO = 3,    // General information
    LOG_DEBUG = 4    // Detailed debug output
};

/**
 * @brief Lightweight logger class for module-specific logging
 */
class Logger {
private:
    const char* module_name_;
    static LogLevel global_level_;
    static bool check_usb_;  // Enable USB checking for power savings
    
    /**
     * @brief Template for unified logging implementation
     */
    template<LogLevel Level>
    void log(const char* prefix, const char* fmt, va_list args) const {
        // Skip logging if USB checking is enabled and no USB connection
        if (check_usb_ && !stdio_usb_connected()) {
            return;
        }
        
        if (global_level_ >= Level) {
            printf("%s [%s]: ", prefix, module_name_);
            vprintf(fmt, args);
            printf("\n");
        }
    }
    
public:
    /**
     * @brief Create a logger for a specific module
     * @param module_name Name of the module (should be static string)
     */
    explicit Logger(const char* module_name) : module_name_(module_name) {}
    
    /**
     * @brief Log an error message
     */
    void error(const char* fmt, ...) const {
        va_list args;
        va_start(args, fmt);
        log<LOG_ERROR>("ERR", fmt, args);
        va_end(args);
    }
    
    /**
     * @brief Log a warning message
     */
    void warn(const char* fmt, ...) const {
        va_list args;
        va_start(args, fmt);
        log<LOG_WARN>("WARN", fmt, args);
        va_end(args);
    }
    
    /**
     * @brief Log an informational message
     */
    void info(const char* fmt, ...) const {
        va_list args;
        va_start(args, fmt);
        log<LOG_INFO>("INFO", fmt, args);
        va_end(args);
    }
    
    /**
     * @brief Log a debug message
     */
    void debug(const char* fmt, ...) const {
        va_list args;
        va_start(args, fmt);
        log<LOG_DEBUG>("DBG", fmt, args);
        va_end(args);
    }
    
    /**
     * @brief Set the global log level
     */
    static void setLogLevel(LogLevel level) { global_level_ = level; }
    
    /**
     * @brief Get the current log level
     */
    static LogLevel getLogLevel() { return global_level_; }
    
    /**
     * @brief Enable/disable USB connection checking for power savings
     * @param enable If true, only log when USB is connected
     */
    static void checkForUsbConnection(bool enable) { check_usb_ = enable; }
};