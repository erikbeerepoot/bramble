/**
 ******************************************************************************
 * @file           : led.h
 * @brief          : LED control class
 ******************************************************************************
 */

#ifndef LED_H
#define LED_H

#include "main.h"

#ifdef USE_WS2812

/**
 * @brief LED controller class for WS2812 addressable RGB LED
 *
 * Drives a WS2812 LED using GPIO bit-banging.
 * Interrupts are briefly disabled (~30us) during each transmission.
 */
class LED {
public:
    enum Color {
        OFF = 0,
        RED = 1,
        GREEN = 2,
        ORANGE = 3
    };

    LED(GPIO_TypeDef *port, uint16_t pin);

    void init();
    void setColor(Color color);
    void setRGB(uint8_t red, uint8_t green, uint8_t blue);
    void blink(Color color, uint32_t duration_ms);
    void off();

private:
    static constexpr uint8_t DATA_BITS = 24;  // 8 green + 8 red + 8 blue

    GPIO_TypeDef *port;
    uint16_t pin;
    uint8_t dmaBuffer[DATA_BITS] = {0};

    void fillBuffer(uint8_t green, uint8_t red, uint8_t blue);
    void sendData();
};

#else  // !USE_WS2812

/**
 * @brief LED controller class for dual-color GPIO LED
 *
 * Supports three colors: red, green, and orange (red + green)
 * Common cathode: HIGH = ON, LOW = OFF
 */
class LED {
public:
    enum Color {
        OFF = 0,
        RED = 1,
        GREEN = 2,
        ORANGE = 3
    };

    LED();

    void init();
    void setColor(Color color);
    void blink(Color color, uint32_t duration_ms);
    void off();

private:
    void setRed(bool on);
    void setGreen(bool on);
};

#endif  // USE_WS2812

#endif /* LED_H */
