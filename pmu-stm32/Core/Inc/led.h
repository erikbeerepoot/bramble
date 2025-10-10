/**
 ******************************************************************************
 * @file           : led.h
 * @brief          : LED control class
 ******************************************************************************
 */

#ifndef LED_H
#define LED_H

#include "main.h"

/**
 * @brief LED controller class for common anode RGB LED
 *
 * Supports three colors: red, green, and orange (red + green)
 * Common anode means LOW = ON, HIGH = OFF
 */
class LED {
public:
    /**
     * @brief LED color enumeration
     */
    enum Color {
        OFF    = 0,
        RED    = 1,
        GREEN  = 2,
        ORANGE = 3
    };

    /**
     * @brief Construct LED controller
     */
    LED();

    /**
     * @brief Initialize the LED hardware
     * @note Should be called after GPIO initialization
     */
    void init();

    /**
     * @brief Set LED to a specific color
     * @param color The color to set
     */
    void setColor(Color color);

    /**
     * @brief Blink the LED with a specific color
     * @param color The color to blink
     * @param duration_ms Duration to keep the LED on in milliseconds
     */
    void blink(Color color, uint32_t duration_ms);

    /**
     * @brief Turn off the LED
     */
    void off();

private:
    void setRed(bool on);
    void setGreen(bool on);
};

#endif /* LED_H */
