#include "neopixel.h"
#include <cstring>

NeoPixel::NeoPixel(uint pin, uint num_pixels) : pio_pin(pin)
{
    pixels.resize(num_pixels, {0, 0, 0}); // Initialize all pixels to black

    // Determine which PIO and state machine to use
    bool pio0_available = false;
    bool pio1_available = false;

    // Check for available state machines
    for (int i = 0; i < 4; i++)
    {
        if (!pio_sm_is_claimed(pio0, i))
        {
            pio = pio0;
            sm = i;
            pio0_available = true;
            break;
        }
    }

    if (!pio0_available)
    {
        for (int i = 0; i < 4; i++)
        {
            if (!pio_sm_is_claimed(pio1, i))
            {
                pio = pio1;
                sm = i;
                pio1_available = true;
                break;
            }
        }
    }

    if (!pio0_available && !pio1_available)
    {
        // No state machines available
        return;
    }

    // Claim the state machine
    pio_sm_claim(pio, sm);

    // Load the program
    offset = pio_add_program(pio, &ws2812_program);
    ws2812_program_init(pio, sm, offset, pio_pin, WS2812_FREQ, false);

    initialized = true;
}

NeoPixel::~NeoPixel()
{
    if (initialized)
    {
        clear();
        show();
        pio_sm_set_enabled(pio, sm, false);
        pio_sm_unclaim(pio, sm);
    }
    // No need to free memory - std::vector handles it
}

void NeoPixel::setPixel(uint index, uint8_t r, uint8_t g, uint8_t b)
{
    if (index < pixels.size())
    {
        pixels[index] = {r, g, b};
    }
}

void NeoPixel::setPixel(uint index, neopixel_color_t color)
{
    if (index < pixels.size())
    {
        pixels[index] = color;
    }
}

void NeoPixel::clear()
{
    std::fill(pixels.begin(), pixels.end(), neopixel_color_t{0, 0, 0});
}

void NeoPixel::show()
{
    if (!initialized)
        return;

    for (const auto &pixel : pixels)
    {
        // Apply brightness scaling during output
        uint8_t r = (pixel.r * global_brightness) / 255;
        uint8_t g = (pixel.g * global_brightness) / 255;
        uint8_t b = (pixel.b * global_brightness) / 255;

        // WS2812 expects GRB format
        uint32_t pixel_data = ((uint32_t)g << 16) | ((uint32_t)r << 8) | (uint32_t)b;
        pio_sm_put_blocking(pio, sm, pixel_data << 8);
    }
}

void NeoPixel::setBrightness(uint8_t brightness)
{
    global_brightness = brightness;
}

uint NeoPixel::getNumPixels() const
{
    return pixels.size();
}

// Simplified HSV to RGB conversion
neopixel_color_t NeoPixel::colorHSV(uint16_t h, uint8_t s, uint8_t v)
{
    // Scale hue to 0-255 range for simpler calculation
    uint8_t hue = h >> 8;

    if (s == 0)
    {
        // Grayscale
        return {v, v, v};
    }

    uint8_t region = hue / 43; // 256 / 6 regions
    uint8_t remainder = (hue - (region * 43)) * 6;

    uint8_t p = (v * (255 - s)) >> 8;
    uint8_t q = (v * (255 - ((s * remainder) >> 8))) >> 8;
    uint8_t t = (v * (255 - ((s * (255 - remainder)) >> 8))) >> 8;

    switch (region)
    {
    case 0:
        return {v, t, p};
    case 1:
        return {q, v, p};
    case 2:
        return {p, v, t};
    case 3:
        return {p, q, v};
    case 4:
        return {t, p, v};
    default:
        return {v, p, q};
    }
}