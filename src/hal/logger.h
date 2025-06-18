#pragma once
#include <cstdio>
#include <cstdarg>

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
 * @brief Logger class for module-specific logging with power optimization
 */
class Logger {
private:
    const char* module_name_;
    
    // Global settings shared across all logger instances
    static LogLevel global_level_;
    static bool usb_check_enabled_;
    static bool usb_connected_;
    
    /**
     * @brief Internal logging method
     * @param level Log level for this message
     * @param prefix Log level prefix string
     * @param fmt Format string
     * @param args Variable arguments
     */
    void logInternal(LogLevel level, const char* prefix, const char* fmt, va_list args) const;
    
public:
    /**
     * @brief Create a logger for a specific module
     * @param module_name Name of the module (should be static string)
     */
    explicit Logger(const char* module_name);
    
    /**
     * @brief Log an error message (always shown unless LOG_NONE)
     * @param fmt Printf-style format string
     * @param ... Variable arguments
     */
    void error(const char* fmt, ...) const;
    
    /**
     * @brief Log a warning message
     * @param fmt Printf-style format string
     * @param ... Variable arguments
     */
    void warn(const char* fmt, ...) const;
    
    /**
     * @brief Log an informational message
     * @param fmt Printf-style format string
     * @param ... Variable arguments
     */
    void info(const char* fmt, ...) const;
    
    /**
     * @brief Log a debug message
     * @param fmt Printf-style format string
     * @param ... Variable arguments
     */
    void debug(const char* fmt, ...) const;
    
    // Global configuration methods (static)
    
    /**
     * @brief Set the global log level for all logger instances
     * @param level New log level
     */
    static void setLogLevel(LogLevel level);
    
    /**
     * @brief Get the current global log level
     * @return Current log level
     */
    static LogLevel getLogLevel();
    
    /**
     * @brief Convert log level to human-readable string
     * @param level Log level to convert
     * @return String representation
     */
    static const char* logLevelToString(LogLevel level);
    
    /**
     * @brief Enable/disable USB connection checking for power optimization
     * @param enabled If true, only log when USB is connected
     */
    static void setUsbCheckEnabled(bool enabled);
    
    /**
     * @brief Check if USB serial is connected (for power optimization)
     * Should be called periodically to update USB status
     */
    static void updateUsbStatus();
    
    /**
     * @brief Check if logging is currently enabled
     * @param level Log level to check
     * @return true if messages at this level should be logged
     */
    static bool isLoggingEnabled(LogLevel level);
};