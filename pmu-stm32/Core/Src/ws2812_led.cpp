/**
 ******************************************************************************
 * @file           : ws2812_led.cpp
 * @brief          : WS2812 addressable RGB LED driver using GPIO bit-banging
 *
 * Drives a single WS2812 LED on PA5 using direct GPIO register writes.
 * Interrupts are disabled for ~30us during each 24-bit transmission.
 *
 * Timing at 16 MHz (62.5 ns/cycle):
 *   Bit "1": ~687ns high, ~562ns low  (within WS2812 spec)
 *   Bit "0": ~250ns high, ~1000ns low (within WS2812 spec)
 *   Both paths: same total cycle count for consistent bit period
 ******************************************************************************
 */

#ifdef USE_WS2812

#include "led.h"

LED::LED()
{
}

void LED::init()
{
    // Configure PA5 as fast GPIO output for bit-banging
    GPIO_InitTypeDef gpioInit = {0};
    gpioInit.Pin = GPIO_PIN_5;
    gpioInit.Mode = GPIO_MODE_OUTPUT_PP;
    gpioInit.Pull = GPIO_NOPULL;
    gpioInit.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    HAL_GPIO_Init(GPIOA, &gpioInit);

    GPIOA->BRR = GPIO_PIN_5;
    HAL_Delay(1);  // >50us reset

    off();
}

void LED::setColor(Color color)
{
    switch (color) {
        case RED:
            setRGB(255, 0, 0);
            break;
        case GREEN:
            setRGB(0, 255, 0);
            break;
        case ORANGE:
            setRGB(255, 165, 0);
            break;
        case OFF:
        default:
            setRGB(0, 0, 0);
            break;
    }
}

void LED::setRGB(uint8_t red, uint8_t green, uint8_t blue)
{
    fillBuffer(green, red, blue);  // WS2812 expects GRB order
    sendData();
}

void LED::blink(Color color, uint32_t duration_ms)
{
    setColor(color);
    HAL_Delay(duration_ms);
    off();
}

void LED::off()
{
    setRGB(0, 0, 0);
}

void LED::fillBuffer(uint8_t green, uint8_t red, uint8_t blue)
{
    uint32_t grb = ((uint32_t)green << 16) | ((uint32_t)red << 8) | blue;

    for (int i = 0; i < DATA_BITS; i++) {
        dmaBuffer[i] = (grb & (1 << (DATA_BITS - 1 - i))) ? 1 : 0;
    }
}

/**
 * @brief Send one byte MSB-first using inline bit-banging
 *
 * Both bit paths have the same total NOP count (9) so the bit period
 * is consistent regardless of data content. The only difference is
 * where the BRR (set-low) write falls within the cycle.
 *
 * At 16 MHz (62.5 ns/cycle):
 *   Bit "1": BSRR + 9 NOPs + BRR = ~687ns high, then loop overhead ~562ns low
 *   Bit "0": BSRR + 2 NOPs + BRR + 7 NOPs = ~250ns high, then loop overhead ~562ns low
 */
__attribute__((noinline, optimize("O1")))
static void sendByte(uint8_t byte)
{
    for (int bit = 7; bit >= 0; bit--) {
        if (byte & (1 << bit)) {
            // "1" bit: long high, short low
            GPIOA->BSRR = GPIO_PIN_5;
            __NOP(); __NOP(); __NOP(); __NOP(); __NOP();
            __NOP(); __NOP(); __NOP(); __NOP();
            GPIOA->BRR = GPIO_PIN_5;
            // 0 extra NOPs — loop overhead provides the low time
        } else {
            // "0" bit: short high, long low
            GPIOA->BSRR = GPIO_PIN_5;
            __NOP(); __NOP();
            GPIOA->BRR = GPIO_PIN_5;
            __NOP(); __NOP(); __NOP(); __NOP(); __NOP();
            __NOP(); __NOP();
        }
    }
}

void LED::sendData()
{
    // Extract GRB bytes from buffer
    uint8_t green = 0, red = 0, blue = 0;
    for (int i = 0; i < 8; i++) {
        green |= (dmaBuffer[i] << (7 - i));
        red   |= (dmaBuffer[8 + i] << (7 - i));
        blue  |= (dmaBuffer[16 + i] << (7 - i));
    }

    __disable_irq();

    sendByte(green);
    sendByte(red);
    sendByte(blue);

    __enable_irq();

    // Reset: hold low for >50us to latch data
    // Use cycle-counting delay instead of HAL_Delay so this is safe
    // from ISR context (HAL_Delay depends on SysTick which may not fire)
    // At 16 MHz, 1000 iterations ≈ ~250us (well above 50us minimum)
    for (volatile uint32_t i = 0; i < 1000; i++) {}
}

// DMA IRQ handler stub — kept for ISR vector compatibility
extern "C" void WS2812_DMA_IRQHandler(void)
{
}

#endif  // USE_WS2812
