#include "application_mode.h"

#include "pico/stdlib.h"

#include "hardware/watchdog.h"

#include "hal/logger.h"
#include "lora/address_manager.h"
#include "lora/hub_router.h"
#include "lora/network_stats.h"
#include "lora/reliable_messenger.h"
#include "lora/sx1276.h"

static Logger logger("AppMode");

// Forward declaration of common message processing function
extern void processIncomingMessage(uint8_t *rx_buffer, int rx_len, ReliableMessenger &messenger,
                                   AddressManager *address_manager, HubRouter *hub_router,
                                   uint32_t current_time, NetworkStats *network_stats,
                                   SX1276 *lora);

void ApplicationMode::run()
{
    // Initialize RTC for all nodes
    rtc_init();
    logger.debug("RTC initialized");

    // Set up actuator command callback
    messenger_.setActuatorCallback(
        [this](const ActuatorPayload *payload) { onActuatorCommand(payload); });

    // Set up update available callback
    messenger_.setUpdateCallback(
        [this](const UpdateAvailablePayload *payload) { onUpdateAvailable(payload); });

    // Set up heartbeat response callback
    messenger_.setHeartbeatResponseCallback(
        [this](const HeartbeatResponsePayload *payload) { onHeartbeatResponse(payload); });

    // Note: Reregistration callback should be set up by main.cpp which has access
    // to the ConfigurationManager needed to clear the saved address before rebooting.

    // Call startup hook
    onStart();

    // Mark initialization complete - state machine can now transition based on hardware
    state_machine_.markInitialized();
    updateStateMachine();

    // If using multicore, start the task manager on Core 1
    task_manager_.start();

    // Add LED update task if pattern exists
    if (led_pattern_) {
        task_manager_.addTask([this](uint32_t time) { updateLED(time); },
                              10,  // Update every 10ms for smooth animation
                              "LED Update");
    }

    // Main loop
    while (true) {
        uint32_t current_time = to_ms_since_boot(get_absolute_time());

        // Call mode-specific loop hook
        onLoop();

        // Update tasks (only if not using multicore)
        task_manager_.update(current_time);

        // Check for interrupts first (more efficient than polling)
        if (lora_.isInterruptPending()) {
            lora_.handleInterrupt();
        }

        // Fallback: check for missed RX interrupts via register poll
        lora_.checkForMissedRxInterrupt();

        // Check for incoming messages
        if (lora_.isMessageReady()) {
            uint8_t rx_buffer[MESSAGE_MAX_SIZE];
            int rx_len = lora_.receive(rx_buffer, sizeof(rx_buffer));

            if (rx_len > 0) {
                processIncomingMessage(rx_buffer, rx_len, current_time);
            } else if (rx_len < 0) {
                lora_.startReceive();
            }
        }

        // Update retry timers for reliable message delivery
        messenger_.update();

        // Update hub router if in hub mode
        if (hub_router_) {
            hub_router_->processQueuedMessages();
        }

        // Brief sleep between iterations
        if (shouldSleep() && !lora_.isInterruptPending()) {
            sleep_ms(10);
        }
    }
}

void ApplicationMode::processIncomingMessage(uint8_t *rx_buffer, int rx_len, uint32_t current_time)
{
    // Use the common message processing function
    ::processIncomingMessage(rx_buffer, rx_len, messenger_, address_manager_, hub_router_,
                             current_time, network_stats_, &lora_);
}

void ApplicationMode::onHeartbeatResponse(const HeartbeatResponsePayload *payload)
{
    if (!payload)
        return;

    // Log pending update flags if any are set
    if (payload->pending_update_flags != PENDING_FLAG_NONE) {
        logger.info("Heartbeat response has pending flags: 0x%02X", payload->pending_update_flags);
    }

    // Convert HeartbeatResponsePayload to datetime_t
    datetime_t dt;
    dt.year = payload->year;
    dt.month = payload->month;
    dt.day = payload->day;
    dt.dotw = payload->dotw;
    dt.hour = payload->hour;
    dt.min = payload->min;
    dt.sec = payload->sec;

    // Set RP2040 RTC
    if (rtc_set_datetime(&dt)) {
        // Wait for RTC to propagate, sync logger timestamp, then update state machine
        sleep_us(64);
        Logger::syncSubsecondCounter();
        updateStateMachine();
        logger.info("RTC synchronized: %04d-%02d-%02d %02d:%02d:%02d (dow=%d)", dt.year, dt.month,
                    dt.day, dt.hour, dt.min, dt.sec, dt.dotw);

        // Switch to operational LED pattern now that we're initialized
        switchToOperationalPattern();
    } else {
        logger.error("Failed to set RTC");
    }

    // Handle re-registration flag (applies to all node types)
    if (payload->pending_update_flags & PENDING_FLAG_REREGISTER) {
        logger.warn("Hub requests re-registration (PENDING_FLAG_REREGISTER)");
        if (reregistration_callback_) {
            reregistration_callback_();
        }
    }

    // Handle reboot flag (applies to all node types)
    if (payload->pending_update_flags & PENDING_FLAG_REBOOT) {
        logger.warn("Hub requests reboot (PENDING_FLAG_REBOOT)");
        onRebootRequested();
    }
}

uint32_t ApplicationMode::getUnixTimestamp() const
{
    datetime_t dt;
    if (!rtc_get_datetime(&dt)) {
        return 0;
    }

    // Convert datetime to Unix timestamp
    // Days from year 1970 to current year
    uint32_t days = 0;
    for (int y = 1970; y < dt.year; y++) {
        days += (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0)) ? 366 : 365;
    }

    // Days from months in current year
    static const uint16_t days_before_month[] = {0,   31,  59,  90,  120, 151,
                                                 181, 212, 243, 273, 304, 334};
    days += days_before_month[dt.month - 1];

    // Add leap day if applicable
    if (dt.month > 2 && (dt.year % 4 == 0 && (dt.year % 100 != 0 || dt.year % 400 == 0))) {
        days++;
    }

    // Add days in current month
    days += dt.day - 1;

    // Convert to seconds and add time
    return days * 86400UL + dt.hour * 3600UL + dt.min * 60UL + dt.sec;
}

void ApplicationMode::switchToOperationalPattern()
{
    if (operational_pattern_) {
        led_pattern_ = std::move(operational_pattern_);
        logger.info("Switched to operational LED pattern");
    }
}

void ApplicationMode::updateStateMachine()
{
    BaseHardwareState hardware_state;
    hardware_state.rtc_running = rtc_running();
    state_machine_.update(hardware_state);
}

void ApplicationMode::onRebootRequested()
{
    logger.warn("Performing RP2040-only watchdog reboot (no PMU)");
    watchdog_reboot(0, 0, 0);
}
