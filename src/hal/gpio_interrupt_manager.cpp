#include "gpio_interrupt_manager.h"

#include "hal/logger.h"

static Logger log("GPIO_IRQ");

GpioInterruptManager &GpioInterruptManager::getInstance()
{
    static GpioInterruptManager instance;
    return instance;
}

GpioInterruptManager::GpioInterruptManager()
{
    // Constructor - nothing to do here yet
}

void GpioInterruptManager::registerHandler(uint gpio, uint32_t events, InterruptHandler handler)
{
    if (gpio >= MAX_GPIO_PINS) {
        log.warn("Cannot register handler for invalid GPIO pin %d", gpio);
        return;
    }

    // Store the handler
    handlers_[gpio] = handler;

    // Enable interrupts for this pin
    if (!global_handler_registered_) {
        // First registration - set up the global callback
        gpio_set_irq_enabled_with_callback(gpio, events, true,
                                           &GpioInterruptManager::globalInterruptHandler);
        global_handler_registered_ = true;
        log.debug("Registered global handler with pin %d", gpio);
    } else {
        // Subsequent registrations - just enable interrupts for this pin
        gpio_set_irq_enabled(gpio, events, true);
        log.debug("Enabled interrupts for pin %d", gpio);
    }
}

void GpioInterruptManager::unregisterHandler(uint gpio)
{
    if (gpio >= MAX_GPIO_PINS) {
        log.warn("Cannot unregister handler for invalid GPIO pin %d", gpio);
        return;
    }

    // Disable interrupts for this pin
    gpio_set_irq_enabled(gpio, GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL, false);

    // Remove the handler
    handlers_[gpio] = nullptr;

    log.debug("Unregistered handler for pin %d", gpio);
}

void GpioInterruptManager::globalInterruptHandler(uint gpio, uint32_t events)
{
    // IMPORTANT: This runs in hardware interrupt context.
    // No logging, no printf, no USB/UART access, no heap allocation.
    auto &manager = getInstance();
    if (gpio < MAX_GPIO_PINS && manager.handlers_[gpio]) {
        manager.handlers_[gpio](gpio, events);
    }
}