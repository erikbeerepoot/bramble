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
    // Store the handler
    handlers_[gpio] = handler;

    // Enable interrupts for this pin
    if (!global_handler_registered_) {
        // First registration - set up the global callback
        gpio_set_irq_enabled_with_callback(gpio, events, true,
                                           &GpioInterruptManager::globalInterruptHandler);
        global_handler_registered_ = true;
        log.info("Registered global handler with pin %d", gpio);
    } else {
        // Subsequent registrations - just enable interrupts for this pin
        gpio_set_irq_enabled(gpio, events, true);
        log.debug("Enabled interrupts for pin %d", gpio);
    }
}

void GpioInterruptManager::unregisterHandler(uint gpio)
{
    // Disable interrupts for this pin
    gpio_set_irq_enabled(gpio, GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL, false);

    // Remove the handler
    handlers_.erase(gpio);

    log.debug("Unregistered handler for pin %d", gpio);
}

void GpioInterruptManager::globalInterruptHandler(uint gpio, uint32_t events)
{
    log.debug("Global handler called for pin %d (events: 0x%X)", gpio, events);

    // Get the singleton instance and route the interrupt
    auto &manager = getInstance();

    // Find the handler for this GPIO
    auto it = manager.handlers_.find(gpio);
    if (it != manager.handlers_.end()) {
        log.debug("Routing to registered handler for pin %d", gpio);
        // Call the registered handler
        it->second(gpio, events);
    } else {
        log.warn("Unhandled interrupt on pin %d (events: 0x%X)", gpio, events);
    }
}