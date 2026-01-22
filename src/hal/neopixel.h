#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

#include "hardware/clocks.h"
#include "hardware/pio.h"

#include "ws2812.pio.h"

/**
 * @brief Simple RGB color structure
 */
struct neopixel_color_t {
    uint8_t r;
    uint8_t g;
    uint8_t b;
};

/**
 * @brief Hardware abstraction for WS2812 NeoPixel LEDs using PIO
 *
 * This class provides a simple interface to control a strip of WS2812 RGB LEDs
 * using the RP2040's PIO (Programmable I/O) peripheral for precise timing.
 */
class NeoPixel {
public:
    /**
     * @brief Construct a new NeoPixel controller
     * @param pin GPIO pin connected to the NeoPixel data line
     * @param num_pixels Number of pixels in the strip
     */
    NeoPixel(uint pin, uint num_pixels);

    /**
     * @brief Destructor - cleans up PIO resources
     */
    ~NeoPixel();

    /**
     * @brief Set a single pixel color
     * @param index Pixel index (0 to num_pixels-1)
     * @param r Red value (0-255)
     * @param g Green value (0-255)
     * @param b Blue value (0-255)
     */
    void setPixel(uint index, uint8_t r, uint8_t g, uint8_t b);

    /**
     * @brief Set a single pixel color using color struct
     * @param index Pixel index (0 to num_pixels-1)
     * @param color RGB color structure
     */
    void setPixel(uint index, neopixel_color_t color);

    /**
     * @brief Clear all pixels (set to black)
     */
    void clear();

    /**
     * @brief Update the LED strip with current pixel values
     * @note This must be called to make color changes visible
     */
    void show();

    /**
     * @brief Set global brightness for all pixels
     * @param brightness Brightness value (0-255)
     * @note This affects all pixels when show() is called
     */
    void setBrightness(uint8_t brightness);

    /**
     * @brief Get the number of pixels
     * @return Number of pixels in the strip
     */
    uint getNumPixels() const;

    /**
     * @brief Convert HSV color to RGB
     * @param h Hue (0-65535, full color wheel)
     * @param s Saturation (0-255)
     * @param v Value/brightness (0-255)
     * @return RGB color structure
     */
    static neopixel_color_t colorHSV(uint16_t h, uint8_t s, uint8_t v);

    /**
     * @brief Helper to create color from components
     * @param r Red value (0-255)
     * @param g Green value (0-255)
     * @param b Blue value (0-255)
     * @return RGB color structure
     */
    static neopixel_color_t color(uint8_t r, uint8_t g, uint8_t b) { return {r, g, b}; }

private:
    static constexpr uint32_t WS2812_FREQ = 800000;  // 800kHz for WS2812

    uint pio_pin;
    PIO pio;
    uint sm;
    uint offset;

    std::vector<neopixel_color_t> pixels;
    uint8_t global_brightness = 255;  // Store brightness separately

    bool initialized = false;
};