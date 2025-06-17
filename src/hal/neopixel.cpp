#include "neopixel.h"
#include "hardware/pio.h"
#include "hardware/clocks.h"
#include "ws2812.pio.h"
#include <cstdlib>
#include <cstring>
#include <algorithm>

NeoPixel::NeoPixel(int pin, int num_pixels) 
    : pin(pin), num_pixels(num_pixels), sm(0), offset(0), pixels(nullptr) {
    pixels = (neopixel_color_t*)malloc(num_pixels * sizeof(neopixel_color_t));
    if (pixels) {
        clear();
    }
}

NeoPixel::~NeoPixel() {
    if (pixels) {
        free(pixels);
    }
}

bool NeoPixel::begin() {
    if (!pixels) return false;
    
    PIO pio = pio0;
    
    // Try to claim a state machine
    this->sm = pio_claim_unused_sm(pio, true);
    if (this->sm == -1) {
        // Try pio1 if pio0 is full
        pio = pio1;
        this->sm = pio_claim_unused_sm(pio, true);
        if (this->sm == -1) {
            return false;
        }
    }
    
    // Load the PIO program
    this->offset = pio_add_program(pio, &ws2812_program);
    
    // Initialize the state machine
    ws2812_program_init(pio, this->sm, this->offset, this->pin, 800000, false);
    
    return true;
}

void NeoPixel::setPixelColor(int pixel_index, uint8_t r, uint8_t g, uint8_t b) {
    if (pixel_index >= this->num_pixels || !pixels) return;
    
    pixels[pixel_index].r = r;
    pixels[pixel_index].g = g;
    pixels[pixel_index].b = b;
}

void NeoPixel::setPixelColor(int pixel_index, neopixel_color_t color) {
    if (pixel_index >= this->num_pixels || !pixels) return;
    
    pixels[pixel_index] = color;
}

void NeoPixel::clear() {
    if (!pixels) return;
    
    for (uint i = 0; i < this->num_pixels; i++) {
        pixels[i].r = 0;
        pixels[i].g = 0;
        pixels[i].b = 0;
    }
}

void NeoPixel::show() {
    if (!pixels) return;
    
    PIO pio = (this->sm < 4) ? pio0 : pio1;
    uint actual_sm = this->sm & 3;
    
    for (uint i = 0; i < this->num_pixels; i++) {
        // WS2812 expects GRB format
        uint32_t pixel_grb = ((uint32_t)pixels[i].g << 16) | 
                            ((uint32_t)pixels[i].r << 8) | 
                            pixels[i].b;
        
        pio_sm_put_blocking(pio, actual_sm, pixel_grb << 8u);
    }
}

void NeoPixel::setBrightness(float brightness) {
    brightness = std::max(0.0f, std::min(1.0f, brightness));
    
    if (!pixels) return;
    
    for (uint i = 0; i < this->num_pixels; i++) {
        pixels[i].r = (uint8_t)(pixels[i].r * brightness);
        pixels[i].g = (uint8_t)(pixels[i].g * brightness);
        pixels[i].b = (uint8_t)(pixels[i].b * brightness);
    }
}

neopixel_color_t NeoPixel::colorRGB(uint8_t r, uint8_t g, uint8_t b) {
    neopixel_color_t color;
    color.r = r;
    color.g = g;
    color.b = b;
    return color;
}

neopixel_color_t NeoPixel::colorHSV(uint16_t hue, uint8_t sat, uint8_t val) {
    uint8_t r, g, b;
    
    hue = (hue * 1530L + 32768) / 65536;
    
    if (hue < 510) {
        b = 0;
        if (hue < 255) {
            r = 255;
            g = hue;
        } else {
            r = 510 - hue;
            g = 255;
        }
    } else if (hue < 1020) {
        r = 0;
        if (hue < 765) {
            g = 255;
            b = hue - 510;
        } else {
            g = 1020 - hue;
            b = 255;
        }
    } else if (hue < 1530) {
        g = 0;
        if (hue < 1275) {
            r = hue - 1020;
            b = 255;
        } else {
            r = 255;
            b = 1530 - hue;
        }
    } else {
        r = 255;
        g = b = 0;
    }
    
    uint32_t v1 = 1 + val;
    uint16_t s1 = 1 + sat;
    uint8_t s2 = 255 - sat;
    
    r = ((((r * s1) >> 8) + s2) * v1) >> 8;
    g = ((((g * s1) >> 8) + s2) * v1) >> 8;
    b = ((((b * s1) >> 8) + s2) * v1) >> 8;
    
    return colorRGB(r, g, b);
}