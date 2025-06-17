#ifndef NEOPIXEL_H
#define NEOPIXEL_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
} neopixel_color_t;

class NeoPixel {
private:
    int pin;
    int num_pixels;
    int sm;
    int offset;
    neopixel_color_t* pixels;

public:
    NeoPixel(int pin, int num_pixels);
    ~NeoPixel();
    
    bool begin();
    void setPixelColor(int pixel_index, uint8_t r, uint8_t g, uint8_t b);
    void setPixelColor(int pixel_index, neopixel_color_t color);
    void clear();
    void show();
    void setBrightness(float brightness);
    
    static neopixel_color_t colorRGB(uint8_t r, uint8_t g, uint8_t b);
    static neopixel_color_t colorHSV(uint16_t hue, uint8_t sat, uint8_t val);
};

#endif