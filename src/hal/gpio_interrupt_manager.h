#ifndef GPIO_INTERRUPT_MANAGER_H
#define GPIO_INTERRUPT_MANAGER_H

#include <cstdint>
#include <functional>

#include "pico/stdlib.h"

/**
 * Global GPIO interrupt manager for the Pico SDK
 *
 * The Pico SDK only supports a single global GPIO interrupt callback.
 * This manager provides a way for multiple components to register
 * their own interrupt handlers for different GPIO pins.
 */
class GpioInterruptManager {
public:
    using InterruptHandler = std::function<void(uint gpio, uint32_t events)>;

    /**
     * Get the singleton instance of the interrupt manager
     */
    static GpioInterruptManager &getInstance();

    /**
     * Register a handler for a specific GPIO pin
     * @param gpio The GPIO pin number
     * @param events The events to listen for (GPIO_IRQ_EDGE_RISE, GPIO_IRQ_EDGE_FALL, etc.)
     * @param handler The handler function to call when the interrupt occurs
     */
    void registerHandler(uint gpio, uint32_t events, InterruptHandler handler);

    /**
     * Unregister a handler for a specific GPIO pin
     * @param gpio The GPIO pin number
     */
    void unregisterHandler(uint gpio);

private:
    GpioInterruptManager();
    ~GpioInterruptManager() = default;

    // Prevent copying
    GpioInterruptManager(const GpioInterruptManager &) = delete;
    GpioInterruptManager &operator=(const GpioInterruptManager &) = delete;

    // Static callback function for the Pico SDK
    static void globalInterruptHandler(uint gpio, uint32_t events);

    // Fixed-size array of handlers indexed by GPIO pin number
    // RP2040 has 30 GPIO pins (0-29)
    static constexpr size_t MAX_GPIO_PINS = 30;
    InterruptHandler handlers_[MAX_GPIO_PINS] = {};

    // Flag to track if the global handler has been registered
    bool global_handler_registered_ = false;
};

#endif  // GPIO_INTERRUPT_MANAGER_H