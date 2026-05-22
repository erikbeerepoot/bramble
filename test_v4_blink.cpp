/**
 * Minimal v4 board blink + UART test.
 * Blinks the NeoPixel on GPIO4 between green and blue, prints to UART/USB.
 * Board header: UART1 TX=GPIO24, RX=GPIO25 (115200 baud).
 */
#include "pico/stdlib.h"
#include "hal/neopixel.h"
#include <cstdio>

int main()
{
    stdio_init_all();

    NeoPixel led(PICO_DEFAULT_WS2812_PIN, 1);
    led.setBrightness(32);

    // Solid green while USB/UART settle
    led.setPixel(0, 0, 255, 0);
    led.show();
    sleep_ms(2000);

    // Blue flash to confirm we got past init
    led.setPixel(0, 0, 0, 255);
    led.show();
    sleep_ms(500);

    int count = 0;
    while (true) {
        bool on = (count % 2) == 0;
        led.setPixel(0, 0, on ? 255 : 0, 0);
        led.show();

        printf("v4 board alive, count=%d, led=%s\n", count, on ? "GREEN" : "OFF");
        count++;
        sleep_ms(500);
    }
}
