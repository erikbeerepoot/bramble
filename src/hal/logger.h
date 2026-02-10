#pragma once
#include <cstdarg>
#include <cstdio>

#include "pico/stdlib.h"

#include "hardware/rtc.h"

// Forward declaration to avoid circular dependency
class LogFlashBuffer;

/**
 * @brief Logging levels for debug output control
 */
enum class LogLevel : uint8_t {
    None = 0,   // No logging
    Error = 1,  // Critical errors only
    Warn = 2,   // Warnings and errors
    Info = 3,   // General information
    Debug = 4   // Detailed debug output
};

/**
 * @brief Lightweight logger class for module-specific logging
 *
 * Outputs to stdio (USB/UART) and optionally to a flash-backed log buffer.
 */
class Logger {
private:
    const char *module_name_;
    static LogLevel global_level_;
    static bool check_usb_;        // Enable USB checking for power savings
    static uint64_t rtc_sync_us_;  // System time (us) when RTC was last synced
    static LogFlashBuffer *flash_sink_;
    static LogLevel flash_level_;            // Minimum level for flash logging
    static bool (*usb_connected_fn_)(void);  // Optional USB connection check callback

    /**
     * @brief Get current timestamp (ms since boot)
     */
    static uint32_t getTimestampMs() { return to_ms_since_boot(get_absolute_time()); }

    /**
     * @brief Write to flash sink if configured
     */
    void writeToFlash(LogLevel level, const char *fmt, va_list args) const;

    /**
     * @brief Unified logging implementation
     */
    void log(LogLevel level, const char *prefix, const char *fmt, va_list args) const
    {
        // Write to flash regardless of USB state
        if (flash_sink_ && static_cast<uint8_t>(flash_level_) >= static_cast<uint8_t>(level)) {
            va_list args_copy;
            va_copy(args_copy, args);
            writeToFlash(level, fmt, args_copy);
            va_end(args_copy);
        }

        // Skip console if USB checking is enabled and no USB connection
        if (check_usb_ && usb_connected_fn_ && !usb_connected_fn_()) {
            return;
        }

        if (static_cast<uint8_t>(global_level_) >= static_cast<uint8_t>(level)) {
            // Print timestamp prefix
            datetime_t dt;
            if (rtc_running() && rtc_get_datetime(&dt)) {
                // RTC is running - use datetime format with subsecond precision
                uint32_t ms_in_second = 0;
                if (rtc_sync_us_ > 0) {
                    uint64_t elapsed_us = to_us_since_boot(get_absolute_time()) - rtc_sync_us_;
                    ms_in_second = (elapsed_us / 1000) % 1000;
                }
                printf("[%04d-%02d-%02d %02d:%02d:%02d.%03lu] ", dt.year, dt.month, dt.day, dt.hour,
                       dt.min, dt.sec, ms_in_second);
            } else {
                // RTC not running - use milliseconds since boot
                uint32_t ms = to_ms_since_boot(get_absolute_time());
                printf("[+%lums] ", ms);
            }

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
    explicit Logger(const char *module_name) : module_name_(module_name) {}

    /**
     * @brief Log an error message
     */
    void error(const char *fmt, ...) const
    {
        va_list args;
        va_start(args, fmt);
        log(LogLevel::Error, "ERR", fmt, args);
        va_end(args);
    }

    /**
     * @brief Log a warning message
     */
    void warn(const char *fmt, ...) const
    {
        va_list args;
        va_start(args, fmt);
        log(LogLevel::Warn, "WARN", fmt, args);
        va_end(args);
    }

    /**
     * @brief Log an informational message
     */
    void info(const char *fmt, ...) const
    {
        va_list args;
        va_start(args, fmt);
        log(LogLevel::Info, "INFO", fmt, args);
        va_end(args);
    }

    /**
     * @brief Log a debug message
     */
    void debug(const char *fmt, ...) const
    {
        va_list args;
        va_start(args, fmt);
        log(LogLevel::Debug, "DBG", fmt, args);
        va_end(args);
    }

    /**
     * @brief Set the global console log level
     */
    static void setLogLevel(LogLevel level) { global_level_ = level; }

    /**
     * @brief Get the current console log level
     */
    static LogLevel getLogLevel() { return global_level_; }

    /**
     * @brief Enable/disable USB connection checking for power savings
     * @param enable If true, only log to console when USB is connected
     */
    static void checkForUsbConnection(bool enable) { check_usb_ = enable; }

    /**
     * @brief Set the flash log sink
     * @param buffer LogFlashBuffer instance (nullptr to disable)
     */
    static void setFlashSink(LogFlashBuffer *buffer) { flash_sink_ = buffer; }

    /**
     * @brief Set minimum level for flash logging (independent of console level)
     */
    static void setFlashLogLevel(LogLevel level) { flash_level_ = level; }

    /**
     * @brief Set the USB connection check function
     * @param fn Function returning true when USB serial is connected (nullptr to disable)
     */
    static void setUsbConnectedCheck(bool (*fn)(void)) { usb_connected_fn_ = fn; }

    /**
     * @brief Flush any buffered log data to flash
     *
     * Call before sleep/power-down to avoid losing the page buffer contents.
     */
    static void flushFlashSink();

    /**
     * @brief Call immediately after rtc_set_datetime() to sync subsecond precision
     */
    static void syncSubsecondCounter() { rtc_sync_us_ = to_us_since_boot(get_absolute_time()); }
};
