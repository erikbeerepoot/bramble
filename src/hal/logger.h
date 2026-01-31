#pragma once
#include <cstdarg>
#include <cstdio>

#include "pico/stdlib.h"

#include "hardware/rtc.h"

#if LIB_PICO_STDIO_USB
#include "pico/stdio_usb.h"
#endif

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
 */
class Logger {
private:
    const char *module_name_;
    static LogLevel global_level_;
    static bool check_usb_;      // Enable USB checking for power savings
    static uint64_t rtc_sync_us_;  // System time (us) when RTC was last synced

    /**
     * @brief Unified logging implementation
     */
    void log(LogLevel level, const char *prefix, const char *fmt, va_list args) const
    {
        // Skip logging if USB checking is enabled and no USB connection
#if LIB_PICO_STDIO_USB
        if (check_usb_ && !stdio_usb_connected()) {
            return;
        }
#endif

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
                printf("[%04d-%02d-%02d %02d:%02d:%02d.%03lu] ", dt.year, dt.month, dt.day,
                       dt.hour, dt.min, dt.sec, ms_in_second);
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

    /**
     * @brief Call immediately after rtc_set_datetime() to sync subsecond precision
     *
     * This records the system timer value at the moment the RTC is set,
     * allowing accurate millisecond calculation within each RTC second.
     */
    static void onRtcSynced() { rtc_sync_us_ = to_us_since_boot(get_absolute_time()); }
};