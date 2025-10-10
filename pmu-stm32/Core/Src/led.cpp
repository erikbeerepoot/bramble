/**
 ******************************************************************************
 * @file           : led.cpp
 * @brief          : LED control class implementation
 ******************************************************************************
 */

#include "led.h"

LED::LED() {
    // Constructor - nothing to initialize here
}

void LED::init() {
    // Turn off both LEDs initially
    off();
}

void LED::setColor(Color color) {
    switch (color) {
        case OFF:
            setRed(false);
            setGreen(false);
            break;

        case RED:
            setRed(true);
            setGreen(false);
            break;

        case GREEN:
            setRed(false);
            setGreen(true);
            break;

        case ORANGE:
            setRed(true);
            setGreen(true);
            break;

        default:
            off();
            break;
    }
}

void LED::blink(Color color, uint32_t duration_ms) {
    setColor(color);
    HAL_Delay(duration_ms);
    off();
}

void LED::off() {
    setRed(false);
    setGreen(false);
}

void LED::setRed(bool on) {
    // Common cathode: HIGH = ON, LOW = OFF
    HAL_GPIO_WritePin(GPIOA, LED_RED_PIN, on ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

void LED::setGreen(bool on) {
    // Common cathode: HIGH = ON, LOW = OFF
    HAL_GPIO_WritePin(GPIOA, LED_GREEN_PIN, on ? GPIO_PIN_SET : GPIO_PIN_RESET);
}
