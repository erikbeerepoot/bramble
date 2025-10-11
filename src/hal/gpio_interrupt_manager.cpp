#include "gpio_interrupt_manager.h"
#include <cstdio>

GpioInterruptManager& GpioInterruptManager::getInstance() {
    static GpioInterruptManager instance;
    return instance;
}

GpioInterruptManager::GpioInterruptManager() {
    // Constructor - nothing to do here yet
}

void GpioInterruptManager::registerHandler(uint gpio, uint32_t events, InterruptHandler handler) {
    // Store the handler
    handlers_[gpio] = handler;

    // Enable interrupts for this pin
    if (!global_handler_registered_) {
        // First registration - set up the global callback
        gpio_set_irq_enabled_with_callback(gpio, events, true, &GpioInterruptManager::globalInterruptHandler);
        global_handler_registered_ = true;
        printf("GPIO Interrupt Manager: Registered global handler with pin %d\n", gpio);
    } else {
        // Subsequent registrations - just enable interrupts for this pin
        gpio_set_irq_enabled(gpio, events, true);
        printf("GPIO Interrupt Manager: Enabled interrupts for pin %d\n", gpio);
    }
}

void GpioInterruptManager::unregisterHandler(uint gpio) {
    // Disable interrupts for this pin
    gpio_set_irq_enabled(gpio, GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL, false);

    // Remove the handler
    handlers_.erase(gpio);

    printf("GPIO Interrupt Manager: Unregistered handler for pin %d\n", gpio);
}

void GpioInterruptManager::globalInterruptHandler(uint gpio, uint32_t events) {
    printf("GPIO Interrupt Manager: Global handler called for pin %d (events: 0x%X)\n", gpio, events);

    // Get the singleton instance and route the interrupt
    auto& manager = getInstance();

    // Find the handler for this GPIO
    auto it = manager.handlers_.find(gpio);
    if (it != manager.handlers_.end()) {
        printf("GPIO Interrupt Manager: Routing to registered handler for pin %d\n", gpio);
        // Call the registered handler
        it->second(gpio, events);
    } else {
        printf("GPIO Interrupt Manager: Unhandled interrupt on pin %d (events: 0x%X)\n", gpio, events);
    }
}