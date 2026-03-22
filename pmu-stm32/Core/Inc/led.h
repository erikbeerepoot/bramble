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

#include "stm32l0xx_hal_dma.h"
#include "stm32l0xx_hal_tim.h"

/**
 * @brief LED controller class for WS2812 addressable RGB LED
 *
 * Drives a WS2812 LED on PA5 using TIM2_CH1 PWM + DMA.
 * Data is encoded as variable-duty-cycle PWM at 800 kHz (1.25 us per bit).
 */
class LED {
public:
    /**
     * @brief LED color enumeration (compatible with GPIO LED variant)
     */
    enum Color {
        OFF = 0,
        RED = 1,
        GREEN = 2,
        ORANGE = 3
    };

    LED();

    /**
     * @brief Initialize TIM2, DMA, and GPIO for WS2812 output on PA5
     * @note Should be called after GPIO and clock initialization
     */
    void init();

    /**
     * @brief Set LED to a predefined color
     * @param color The color to set
     */
    void setColor(Color color);

    /**
     * @brief Set LED to an arbitrary RGB color
     * @param red Red intensity (0-255)
     * @param green Green intensity (0-255)
     * @param blue Blue intensity (0-255)
     */
    void setRGB(uint8_t red, uint8_t green, uint8_t blue);

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

    /**
     * @brief Get the DMA handle (needed by DMA ISR)
     */
    DMA_HandleTypeDef *getDmaHandle() { return &hdma; }

    /**
     * @brief Handle DMA transfer complete (called from ISR context)
     */
    void handleDmaComplete();

private:
    static constexpr uint16_t BIT_PERIOD = 19;  // ARR value: 20 counts = 1.25 us at 16 MHz
    static constexpr uint16_t BIT_HIGH = 11;    // CCR for logic 1 (~0.69 us high)
    static constexpr uint16_t BIT_LOW = 6;      // CCR for logic 0 (~0.38 us high)
    static constexpr uint8_t RESET_SLOTS = 1;   // Trailing zero slot
    static constexpr uint8_t DATA_BITS = 24;    // 8 green + 8 red + 8 blue

    TIM_HandleTypeDef htim2;
    DMA_HandleTypeDef hdma;
    uint16_t dmaBuffer[DATA_BITS + RESET_SLOTS];
    volatile bool transferComplete;

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
    /**
     * @brief LED color enumeration
     */
    enum Color {
        OFF = 0,
        RED = 1,
        GREEN = 2,
        ORANGE = 3
    };

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

#endif  // USE_WS2812

#endif /* LED_H */
