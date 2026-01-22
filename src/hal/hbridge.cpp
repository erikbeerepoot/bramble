#include "hbridge.h"

#include "pico/time.h"

#include "hardware/gpio.h"

#include "hal/logger.h"

static Logger log("HBridge");

void HBridge::initialize(uint8_t pin_high_a, uint8_t pin_low_a, uint8_t pin_high_b,
                         uint8_t pin_low_b)
{
    // Store pin assignments
    pin_high_side_a_ = pin_high_a;
    pin_low_side_a_ = pin_low_a;
    pin_high_side_b_ = pin_high_b;
    pin_low_side_b_ = pin_low_b;

    // Thread safety not needed for single-threaded operation

    // Configure all pins as outputs, initially LOW
    configurePin(pin_high_side_a_);
    configurePin(pin_low_side_a_);
    configurePin(pin_high_side_b_);
    configurePin(pin_low_side_b_);

    // Ensure safe state
    setAllPinsLow();

    is_active_ = false;
    initialized_ = true;

    log.info("Initialized - HA:%d LA:%d HB:%d LB:%d", pin_high_a, pin_low_a, pin_high_b, pin_low_b);
}

void HBridge::pulse(Direction dir, uint32_t duration_ms)
{
    if (!initialized_) {
        log.error("Not initialized");
        return;
    }

    // Enforce maximum pulse duration
    if (duration_ms > MAX_PULSE_DURATION_MS) {
        log.warn("Pulse duration %dms exceeds max %dms, clamping", duration_ms,
                 MAX_PULSE_DURATION_MS);
        duration_ms = MAX_PULSE_DURATION_MS;
    }

    // Set active flag
    is_active_ = true;

    // Ensure all MOSFETs are off before switching (safety)
    setAllPinsLow();
    sleep_us(MOSFET_SWITCHING_DELAY_US);

    // Enable appropriate MOSFETs based on direction
    if (dir == FORWARD) {
        // Current flows A->B
        gpio_put(pin_high_side_a_, 1);
        gpio_put(pin_low_side_b_, 1);
        log.info("Pulse FORWARD for %dms", duration_ms);
    } else {
        // Current flows B->A
        gpio_put(pin_high_side_b_, 1);
        gpio_put(pin_low_side_a_, 1);
        log.info("Pulse REVERSE for %dms", duration_ms);
    }

    // Hold for specified duration
    sleep_ms(duration_ms);

    // Turn off all MOSFETs
    setAllPinsLow();

    // Clear active flag
    is_active_ = false;
}

void HBridge::off()
{
    setAllPinsLow();
    is_active_ = false;
}

void HBridge::setAllPinsLow()
{
    gpio_put(pin_high_side_a_, 0);
    gpio_put(pin_low_side_a_, 0);
    gpio_put(pin_high_side_b_, 0);
    gpio_put(pin_low_side_b_, 0);
}

void HBridge::configurePin(uint8_t pin)
{
    gpio_init(pin);
    gpio_set_dir(pin, GPIO_OUT);
    gpio_put(pin, 0);  // Start in OFF state
}