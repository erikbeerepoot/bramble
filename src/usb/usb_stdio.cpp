/**
 * @file usb_stdio.cpp
 * @brief Custom CDC stdio driver for composite CDC+MSC USB device
 *
 * Replaces pico_stdio_usb: provides the same printf-over-USB functionality
 * while coexisting with MSC (mass storage) on the same USB interface.
 *
 * Based on the Pico SDK stdio_usb implementation with simplifications.
 */

#include "usb_stdio.h"

#include "pico/mutex.h"
#include "pico/stdio/driver.h"
#include "pico/time.h"

#include "tusb.h"

// Timeout for blocking writes when CDC buffer is full (500ms)
static constexpr uint32_t STDOUT_TIMEOUT_US = 500000;

// tud_task() polling interval (1ms)
static constexpr int64_t TASK_INTERVAL_US = 1000;

static mutex_t usb_mutex;
static repeating_timer_t usb_timer;

/**
 * @brief Write characters to USB CDC
 *
 * Blocks until all characters are written or timeout expires.
 * If USB CDC is not connected, returns immediately to avoid blocking.
 */
static void usb_out_chars(const char *buf, int length)
{
    uint32_t owner;
    if (!mutex_try_enter(&usb_mutex, &owner)) {
        if (owner == get_core_num())
            return;  // Avoid recursive deadlock
        mutex_enter_blocking(&usb_mutex);
    }

    // Skip entirely if CDC is not connected - avoids blocking when using UART only
    // Check inside mutex to avoid race with tud_task()
    if (!tud_cdc_connected()) {
        mutex_exit(&usb_mutex);
        return;
    }

    uint64_t end = time_us_64() + STDOUT_TIMEOUT_US;
    int pos = 0;

    while (pos < length && time_us_64() < end) {
        uint32_t available = tud_cdc_write_available();
        if (available > 0) {
            uint32_t to_write = (uint32_t)(length - pos);
            if (to_write > available)
                to_write = available;
            uint32_t written = tud_cdc_write(buf + pos, to_write);
            pos += (int)written;
        } else {
            tud_cdc_write_flush();
            tud_task();
        }
    }
    tud_cdc_write_flush();

    mutex_exit(&usb_mutex);
}

/**
 * @brief Flush USB CDC output buffer
 */
static void usb_out_flush(void)
{
    uint32_t owner;
    if (!mutex_try_enter(&usb_mutex, &owner)) {
        if (owner == get_core_num())
            return;
        mutex_enter_blocking(&usb_mutex);
    }

    tud_cdc_write_flush();

    mutex_exit(&usb_mutex);
}

/**
 * @brief Read characters from USB CDC (non-blocking)
 * @return Number of bytes read, or PICO_ERROR_NO_DATA if none available
 */
static int usb_in_chars(char *buf, int length)
{
    uint32_t owner;
    if (!mutex_try_enter(&usb_mutex, &owner)) {
        return PICO_ERROR_NO_DATA;
    }

    int count = 0;
    if (tud_cdc_available()) {
        count = (int)tud_cdc_read(buf, (uint32_t)length);
    }

    mutex_exit(&usb_mutex);
    return count > 0 ? count : PICO_ERROR_NO_DATA;
}

// stdio driver instance
static stdio_driver_t usb_stdio_driver = {
    .out_chars = usb_out_chars,
    .out_flush = usb_out_flush,
    .in_chars = usb_in_chars,
#if PICO_STDIO_ENABLE_CRLF_SUPPORT
    .crlf_enabled = PICO_STDIO_DEFAULT_CRLF,
#endif
};

/**
 * @brief Repeating timer callback - processes USB device tasks
 *
 * Runs from IRQ context every 1ms. Uses non-blocking mutex to avoid
 * contending with application-level CDC writes.
 */
static bool timer_task_callback(repeating_timer_t *rt)
{
    (void)rt;

    uint32_t owner;
    if (mutex_try_enter(&usb_mutex, &owner)) {
        tud_task();
        mutex_exit(&usb_mutex);
    }
    // If mutex is held, skip this tick - next tick will catch up

    return true;  // Keep repeating
}

bool usb_stdio_init(void)
{
    mutex_init(&usb_mutex);

    tusb_init();

    // Repeating timer for tud_task() - negative value means delay between end of
    // callback and start of next call (more stable than fixed-period)
    if (!add_repeating_timer_us(-TASK_INTERVAL_US, timer_task_callback, NULL, &usb_timer)) {
        return false;
    }

    // Register our CDC driver with pico stdio
    stdio_set_driver_enabled(&usb_stdio_driver, true);

    return true;
}

bool usb_stdio_connected(void)
{
    // Use non-blocking mutex check to avoid blocking main loop
    // If mutex is held (USB operation in progress), return false as a safe default
    uint32_t owner;
    if (!mutex_try_enter(&usb_mutex, &owner)) {
        return false;
    }
    // Use tud_ready() instead of tud_cdc_connected() - the latter requires DTR
    // which some terminals don't assert. tud_ready() just checks if USB is enumerated.
    bool connected = tud_ready();
    mutex_exit(&usb_mutex);
    return connected;
}
