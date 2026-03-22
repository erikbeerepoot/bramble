/**
 ******************************************************************************
 * @file           : ws2812_led.cpp
 * @brief          : WS2812 addressable RGB LED driver using TIM2 PWM + DMA
 *
 * Drives a single WS2812 LED on PA5 (TIM2_CH1).
 * DMA writes duty-cycle values to TIM2->CCR1 on each timer update event,
 * producing the variable-width pulses that encode GRB data.
 ******************************************************************************
 */

#ifdef USE_WS2812

#include "led.h"

// File-scope pointer for DMA callback access
static LED *ledInstance = nullptr;

// DMA callbacks (registered on DMA handle, called from HAL_DMA_IRQHandler)
static void dmaTransferComplete(DMA_HandleTypeDef *hdma);
static void dmaTransferError(DMA_HandleTypeDef *hdma);

LED::LED() : transferComplete(true)
{
    ledInstance = this;
}

void LED::init()
{
    // --- DMA1 Channel 2 for TIM2_UP (request 8) ---
    __HAL_RCC_DMA1_CLK_ENABLE();

    hdma.Instance = DMA1_Channel2;
    hdma.Init.Request = DMA_REQUEST_8;
    hdma.Init.Direction = DMA_MEMORY_TO_PERIPH;
    hdma.Init.PeriphInc = DMA_PINC_DISABLE;
    hdma.Init.MemInc = DMA_MINC_ENABLE;
    hdma.Init.PeriphDataAlignment = DMA_PDATAALIGN_WORD;
    hdma.Init.MemDataAlignment = DMA_MDATAALIGN_HALFWORD;
    hdma.Init.Mode = DMA_NORMAL;
    hdma.Init.Priority = DMA_PRIORITY_HIGH;
    HAL_DMA_Init(&hdma);

    // Register DMA callbacks
    hdma.XferCpltCallback = dmaTransferComplete;
    hdma.XferErrorCallback = dmaTransferError;

    HAL_NVIC_SetPriority(DMA1_Channel2_3_IRQn, 2, 0);
    HAL_NVIC_EnableIRQ(DMA1_Channel2_3_IRQn);

    // --- Reconfigure PA5 as TIM2_CH1 (AF5) ---
    GPIO_InitTypeDef gpioInit = {0};
    gpioInit.Pin = GPIO_PIN_5;
    gpioInit.Mode = GPIO_MODE_AF_PP;
    gpioInit.Pull = GPIO_NOPULL;
    gpioInit.Speed = GPIO_SPEED_FREQ_HIGH;
    gpioInit.Alternate = GPIO_AF5_TIM2;
    HAL_GPIO_Init(GPIOA, &gpioInit);

    // --- TIM2 in PWM Mode 1 on Channel 1 ---
    __HAL_RCC_TIM2_CLK_ENABLE();

    htim2.Instance = TIM2;
    htim2.Init.Prescaler = 0;
    htim2.Init.CounterMode = TIM_COUNTERMODE_UP;
    htim2.Init.Period = BIT_PERIOD;  // ARR = 19 -> 20 counts = 1.25 us at 16 MHz
    htim2.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
    htim2.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_ENABLE;
    HAL_TIM_PWM_Init(&htim2);

    TIM_OC_InitTypeDef ocConfig = {0};
    ocConfig.OCMode = TIM_OCMODE_PWM1;
    ocConfig.Pulse = 0;
    ocConfig.OCPolarity = TIM_OCPOLARITY_HIGH;
    ocConfig.OCFastMode = TIM_OCFAST_ENABLE;
    HAL_TIM_OC_ConfigChannel(&htim2, &ocConfig, TIM_CHANNEL_1);

    // Ensure output starts low
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

void LED::handleDmaComplete()
{
    __HAL_TIM_DISABLE_DMA(&htim2, TIM_DMA_UPDATE);
    HAL_TIM_PWM_Stop(&htim2, TIM_CHANNEL_1);
    transferComplete = true;
}

void LED::fillBuffer(uint8_t green, uint8_t red, uint8_t blue)
{
    uint32_t grb = ((uint32_t)green << 16) | ((uint32_t)red << 8) | blue;

    for (int i = 0; i < DATA_BITS; i++) {
        dmaBuffer[i] = (grb & (1 << (DATA_BITS - 1 - i))) ? BIT_HIGH : BIT_LOW;
    }

    // Reset slot: zero duty cycle holds line low
    dmaBuffer[DATA_BITS] = 0;
}

void LED::sendData()
{
    // Wait for any previous transfer to complete (with timeout)
    uint32_t start = HAL_GetTick();
    while (!transferComplete) {
        if (HAL_GetTick() - start > 10) {
            // Timeout — force stop and proceed
            HAL_DMA_Abort(&hdma);
            handleDmaComplete();
            break;
        }
    }

    transferComplete = false;

    // Reset DMA state for a new transfer
    HAL_DMA_Init(&hdma);
    hdma.XferCpltCallback = dmaTransferComplete;
    hdma.XferErrorCallback = dmaTransferError;

    // Start PWM output (pin driven by timer)
    HAL_TIM_PWM_Start(&htim2, TIM_CHANNEL_1);

    // Start DMA: writes dmaBuffer values to TIM2->CCR1 on each update event
    HAL_DMA_Start_IT(&hdma, (uint32_t)dmaBuffer, (uint32_t)&htim2.Instance->CCR1,
                     DATA_BITS + RESET_SLOTS);

    // Enable TIM2 update DMA request
    __HAL_TIM_ENABLE_DMA(&htim2, TIM_DMA_UPDATE);
}

// DMA transfer complete callback (called from HAL_DMA_IRQHandler in ISR context)
static void dmaTransferComplete(DMA_HandleTypeDef *hdma)
{
    (void)hdma;
    if (ledInstance != nullptr) {
        ledInstance->handleDmaComplete();
    }
}

// DMA error callback (called from HAL_DMA_IRQHandler in ISR context)
static void dmaTransferError(DMA_HandleTypeDef *hdma)
{
    (void)hdma;
    if (ledInstance != nullptr) {
        ledInstance->handleDmaComplete();
    }
}

#endif  // USE_WS2812
