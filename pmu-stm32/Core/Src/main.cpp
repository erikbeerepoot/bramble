/* USER CODE BEGIN Header */
/**
 ******************************************************************************
 * @file           : main.c
 * @brief          : Main program body
 ******************************************************************************
 * @attention
 *
 * <h2><center>&copy; Copyright (c) 2025 STMicroelectronics.
 * All rights reserved.</center></h2>
 *
 * This software component is licensed by ST under BSD 3-Clause license,
 * the "License"; You may not use this file except in compliance with the
 * License. You may obtain a copy of the License at:
 *                        opensource.org/licenses/BSD-3-Clause
 *
 ******************************************************************************
 */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "dcdc.h"
#include "led.h"
#include "pmu_protocol.h"
#include "pmu_state_machine.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
UART_HandleTypeDef hlpuart1;

RTC_HandleTypeDef hrtc;

TIM_HandleTypeDef htim21;

/* USER CODE BEGIN PV */
LED led;
DCDC dcdc;

// UART receive buffer
static uint8_t uartRxByte;

// RTC wakeup flag - set by interrupt, handled in main loop
static volatile bool rtcWakeupFlag = false;

// Boot animation parameters
namespace BootAnimationConfig {
constexpr int TOTAL_FLICKERS = 5;  // 5 flickers = 1 second total
constexpr uint32_t FLICKER_ON_MS = 100;
constexpr uint32_t FLICKER_OFF_MS = 100;
}  // namespace BootAnimationConfig

// Boot animation state (for non-blocking animation)
static struct {
    int flickerCount = 0;
    uint32_t lastToggleTime = 0;
    bool ledOn = false;
    bool complete = false;
} bootAnimation;

// Protocol callbacks
static void uartSendCallback(const uint8_t *data, uint8_t length);
static void setWakeIntervalCallback(uint32_t seconds);
static void keepAwakeCallback(uint16_t seconds);
static void readyForSleepCallback();
static uint32_t getTickCallback();

// Protocol instance
static PMU::Protocol protocol(uartSendCallback, setWakeIntervalCallback, keepAwakeCallback,
                              readyForSleepCallback, getTickCallback);

// PMU state machine instance
static PmuStateMachine pmuState(getTickCallback);
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_LPUART1_UART_Init(void);
static void MX_RTC_Init(void);
static void MX_TIM21_Init(void);
/* USER CODE BEGIN PFP */
static void configureRTCWakeup(uint32_t seconds);
static void enterStopMode(void);
static void wakeupFromStopMode(void);
static void determineWakeType(void);
static void startBootAnimation(void);
static void updateLedForState(PmuState state, WakeType wakeType);
static void onStateChange(PmuState newState);
static void sendWakeNotification();
/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

/**
 * @brief  The application entry point.
 * @retval int
 */
int main(void)
{
    /* USER CODE BEGIN 1 */

    /* USER CODE END 1 */

    /* MCU Configuration--------------------------------------------------------*/

    /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
    HAL_Init();

    /* USER CODE BEGIN Init */

    /* USER CODE END Init */

    /* Configure the system clock */
    SystemClock_Config();

    /* Update SystemCoreClock variable after clock configuration */
    SystemCoreClockUpdate();

    /* Reconfigure SysTick with updated clock frequency */
    HAL_InitTick(TICK_INT_PRIORITY);

    /* Enable global interrupts explicitly */
    __enable_irq();

    /* USER CODE BEGIN SysInit */

    /* USER CODE END SysInit */

    /* Initialize all configured peripherals */
    MX_GPIO_Init();
    MX_LPUART1_UART_Init();
    MX_RTC_Init();
    MX_TIM21_Init();

    /* USER CODE BEGIN 2 */
    // Start UART receive interrupt IMMEDIATELY after UART init
    // This ensures PMU is ready to receive commands as early as possible
    HAL_UART_Receive_IT(&hlpuart1, &uartRxByte, 1);

    led.init();
    led.off();

    // Initialize DC/DC converter (start enabled, then we can program the rp2040)
    dcdc.init();
    dcdc.enable();

    // Set up state machine callback for LED updates
    pmuState.setStateCallback(onStateChange);

    // Configure RTC wakeup timer with default wake interval (300 seconds)
    configureRTCWakeup(protocol.getWakeInterval());

    // Start non-blocking boot animation (timer-based)
    // BOOT_COMPLETE is dispatched by timer callback when animation finishes
    // Main loop runs immediately, so CTS can be handled without waiting
    startBootAnimation();

    /* USER CODE END 2 */

    /* Infinite loop */
    /* USER CODE BEGIN WHILE */

    while (1) {
        /* USER CODE END WHILE */

        /* USER CODE BEGIN 3 */

        // =====================================================================
        // Event Processing
        // =====================================================================
        if (rtcWakeupFlag) {
            rtcWakeupFlag = false;
            pmuState.dispatch(PmuEvent::RTC_WAKEUP);
        }

        if (protocol.isCtsReceived()) {
            pmuState.dispatch(PmuEvent::CTS_RECEIVED);
            protocol.clearCtsReceived();
        }

        pmuState.tick();

        // =====================================================================
        // Output Control (pure - no side effects except outputs)
        // =====================================================================
        PmuState state = pmuState.state();
        updateLedForState(state, pmuState.wakeType());

        switch (state) {
            case PmuState::BOOTING:
                // Boot animation handled before loop
                break;

            case PmuState::AWAITING_CTS:
            case PmuState::WAKE_ACTIVE:
                dcdc.enable();
                break;

            case PmuState::SLEEPING:
                dcdc.disable();
                led.setColor(LED::GREEN);
                HAL_Delay(100);
                led.off();
                enterStopMode();
                wakeupFromStopMode();
                break;

            case PmuState::ERROR:
            default:
                dcdc.disable();
                break;
        }
    }
    /* USER CODE END 3 */
}

/**
 * @brief System Clock Configuration
 * @retval None
 */
void SystemClock_Config(void)
{
    RCC_OscInitTypeDef RCC_OscInitStruct = {0};
    RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};
    RCC_PeriphCLKInitTypeDef PeriphClkInit = {0};

    /** Configure the main internal regulator output voltage
     */
    __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

    /** Initializes the RCC Oscillators according to the specified parameters
     * in the RCC_OscInitTypeDef structure.
     */
    RCC_OscInitStruct.OscillatorType =
        RCC_OSCILLATORTYPE_HSI | RCC_OSCILLATORTYPE_LSI | RCC_OSCILLATORTYPE_MSI;
    RCC_OscInitStruct.HSIState = RCC_HSI_ON;  // Enable HSI for LPUART in STOP mode
    RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
    RCC_OscInitStruct.LSIState = RCC_LSI_ON;
    RCC_OscInitStruct.MSIState = RCC_MSI_ON;
    RCC_OscInitStruct.MSICalibrationValue = 0;
    RCC_OscInitStruct.MSIClockRange = RCC_MSIRANGE_5;
    RCC_OscInitStruct.PLL.PLLState = RCC_PLL_NONE;
    if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK) {
        Error_Handler();
    }

    // Explicitly disable HSI divider to ensure 16 MHz clock for LPUART
    // Without this, HSI might be divided by 4 (4 MHz) causing wrong baud rate
    CLEAR_BIT(RCC->CR, RCC_CR_HSIDIVEN);

    /** Initializes the CPU, AHB and APB buses clocks
     */
    RCC_ClkInitStruct.ClockType =
        RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
    RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_MSI;
    RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
    RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV1;
    RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

    if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_0) != HAL_OK) {
        Error_Handler();
    }
    PeriphClkInit.PeriphClockSelection = RCC_PERIPHCLK_LPUART1 | RCC_PERIPHCLK_RTC;
    // Use HSI (16 MHz) for LPUART - more accurate than MSI-derived PCLK1
    // HSI is factory-calibrated with ±1% accuracy over temperature range
    // MSI has wider tolerance which caused baud rate to be ~7.5% off (2218 instead of 2400)
    PeriphClkInit.Lpuart1ClockSelection = RCC_LPUART1CLKSOURCE_HSI;
    PeriphClkInit.RTCClockSelection = RCC_RTCCLKSOURCE_LSI;
    if (HAL_RCCEx_PeriphCLKConfig(&PeriphClkInit) != HAL_OK) {
        Error_Handler();
    }
}

/**
 * @brief LPUART1 Initialization Function
 * @param None
 * @retval None
 */
static void MX_LPUART1_UART_Init(void)
{
    /* USER CODE BEGIN LPUART1_Init 0 */

    /* USER CODE END LPUART1_Init 0 */

    /* USER CODE BEGIN LPUART1_Init 1 */

    /* USER CODE END LPUART1_Init 1 */
    hlpuart1.Instance = LPUART1;
    hlpuart1.Init.BaudRate = 9600;  // Standard rate, requires HSI (16 MHz) clock source
    hlpuart1.Init.WordLength = UART_WORDLENGTH_8B;
    hlpuart1.Init.StopBits = UART_STOPBITS_2;  // 2 stop bits for better timing margin
    hlpuart1.Init.Parity = UART_PARITY_NONE;
    hlpuart1.Init.Mode = UART_MODE_TX_RX;
    hlpuart1.Init.HwFlowCtl = UART_HWCONTROL_NONE;
    hlpuart1.Init.OneBitSampling = UART_ONE_BIT_SAMPLE_DISABLE;
    hlpuart1.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;
    if (HAL_UART_Init(&hlpuart1) != HAL_OK) {
        Error_Handler();
    }
    /* USER CODE BEGIN LPUART1_Init 2 */

    /* USER CODE END LPUART1_Init 2 */
}

/**
 * @brief RTC Initialization Function
 * @param None
 * @retval None
 */
static void MX_RTC_Init(void)
{
    /* USER CODE BEGIN RTC_Init 0 */

    /* USER CODE END RTC_Init 0 */

    /* USER CODE BEGIN RTC_Init 1 */

    /* USER CODE END RTC_Init 1 */

    /** Initialize RTC Only
     */
    hrtc.Instance = RTC;
    hrtc.Init.HourFormat = RTC_HOURFORMAT_24;
    // LSI is ~37kHz, so we need prescalers that divide to 1 Hz for ck_spre
    // 37000 Hz / (125 × 296) = 37000 / 37000 = 1 Hz
    hrtc.Init.AsynchPrediv = 124;  // AsynchPrediv + 1 = 125
    hrtc.Init.SynchPrediv = 295;   // SynchPrediv + 1 = 296
    hrtc.Init.OutPut = RTC_OUTPUT_DISABLE;
    hrtc.Init.OutPutRemap = RTC_OUTPUT_REMAP_NONE;
    hrtc.Init.OutPutPolarity = RTC_OUTPUT_POLARITY_HIGH;
    hrtc.Init.OutPutType = RTC_OUTPUT_TYPE_OPENDRAIN;
    if (HAL_RTC_Init(&hrtc) != HAL_OK) {
        Error_Handler();
    }

    /* USER CODE BEGIN RTC_Init 2 */
    // Wakeup timer will be configured later with interrupt enabled
    /* USER CODE END RTC_Init 2 */
}

/**
 * @brief GPIO Initialization Function
 * @param None
 * @retval None
 */
static void MX_GPIO_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStruct = {0};
    /* USER CODE BEGIN MX_GPIO_Init_1 */

    /* USER CODE END MX_GPIO_Init_1 */

    /* GPIO Ports Clock Enable */
    __HAL_RCC_GPIOA_CLK_ENABLE();

    /*Configure GPIO pin Output Level */
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_1 | GPIO_PIN_4 | GPIO_PIN_5, GPIO_PIN_RESET);

    /*Configure GPIO pins : PA1 PA4 PA5 */
    GPIO_InitStruct.Pin = GPIO_PIN_1 | GPIO_PIN_4 | GPIO_PIN_5;
    GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

    /* USER CODE BEGIN MX_GPIO_Init_2 */

    /* USER CODE END MX_GPIO_Init_2 */
}

/* USER CODE BEGIN 4 */

/**
 * @brief Initialize TIM21 for boot animation
 * Timer fires every 100ms for LED toggle
 */
static void MX_TIM21_Init(void)
{
    __HAL_RCC_TIM21_CLK_ENABLE();

    // MSI clock is 2.097 MHz (range 5)
    // For 100ms period: 2097000 / 100 = 20970 counts per 100ms
    // Use prescaler of 209 (divide by 210) -> 9985.7 Hz
    // Period of 999 (1000 counts) -> 100.1ms (close enough)
    htim21.Instance = TIM21;
    htim21.Init.Prescaler = 209;
    htim21.Init.CounterMode = TIM_COUNTERMODE_UP;
    htim21.Init.Period = 999;
    htim21.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
    htim21.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;

    if (HAL_TIM_Base_Init(&htim21) != HAL_OK) {
        Error_Handler();
    }

    // Enable timer interrupt
    HAL_NVIC_SetPriority(TIM21_IRQn, 1, 0);
    HAL_NVIC_EnableIRQ(TIM21_IRQn);
}

/**
 * @brief Start non-blocking boot animation
 * Uses TIM21 interrupt to toggle LED
 */
static void startBootAnimation(void)
{
    bootAnimation.flickerCount = 0;
    bootAnimation.ledOn = true;
    bootAnimation.complete = false;

    // Start with LED on
    led.setColor(LED::GREEN);

    // Start timer
    HAL_TIM_Base_Start_IT(&htim21);
}

/**
 * @brief TIM21 interrupt handler
 */
extern "C" void TIM21_IRQHandler(void)
{
    HAL_TIM_IRQHandler(&htim21);
}

/**
 * @brief Timer period elapsed callback - handles boot animation
 */
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
    if (htim->Instance != TIM21 || bootAnimation.complete) {
        return;
    }

    if (bootAnimation.ledOn) {
        // LED was on, turn it off
        led.off();
        bootAnimation.ledOn = false;
        bootAnimation.flickerCount++;

        // Check if animation complete
        if (bootAnimation.flickerCount >= BootAnimationConfig::TOTAL_FLICKERS) {
            bootAnimation.complete = true;
            HAL_TIM_Base_Stop_IT(&htim21);
            pmuState.dispatch(PmuEvent::BOOT_COMPLETE);
        }
    } else {
        // LED was off, turn it on
        led.setColor(LED::GREEN);
        bootAnimation.ledOn = true;
    }
}

/**
 * @brief Configure RTC wakeup timer
 * @param seconds Number of seconds between wakeups
 */
static void configureRTCWakeup(uint32_t seconds)
{
    // Disable wakeup timer first
    HAL_RTCEx_DeactivateWakeUpTimer(&hrtc);

    // Wait for WUTWF flag to be set (wakeup timer configuration allowed)
    // This is critical - without it, the SetWakeUpTimer call will hang
    uint32_t timeout = 1000;
    while (!__HAL_RTC_WAKEUPTIMER_GET_FLAG(&hrtc, RTC_FLAG_WUTWF) && timeout > 0) {
        timeout--;
        HAL_Delay(1);
    }

    if (timeout == 0) {
        // WUTWF flag never set - RTC is not ready for configuration
        // Flash LED to indicate error but don't halt
        led.setColor(LED::RED);
        HAL_Delay(2000);
        led.off();
        return;
    }

    // Enable RTC interrupt in NVIC
    HAL_NVIC_SetPriority(RTC_IRQn, 0, 0);
    HAL_NVIC_EnableIRQ(RTC_IRQn);

    // Enable EXTI line 20 (RTC wakeup) for waking from STOP mode
    __HAL_RTC_WAKEUPTIMER_EXTI_ENABLE_IT();
    __HAL_RTC_WAKEUPTIMER_EXTI_ENABLE_RISING_EDGE();

    // Use ck_spre (1 Hz clock) for wakeup timer
    // This allows up to 65535 seconds (~18 hours) with 16-bit counter
    // The counter value is N-1 because it counts from N-1 down to 0
    uint32_t wakeupCounter;

    if (seconds > 65536) {
        // Clamp to maximum (counter goes 0-65535, which is 65536 values)
        wakeupCounter = 65535;
    } else if (seconds > 0) {
        wakeupCounter = seconds - 1;
    } else {
        // Minimum 1 second (counter = 0 means 1 tick)
        wakeupCounter = 0;
    }

    // Enable wakeup timer with interrupt using 1 Hz clock
    if (HAL_RTCEx_SetWakeUpTimer_IT(&hrtc, wakeupCounter, RTC_WAKEUPCLOCK_CK_SPRE_16BITS) !=
        HAL_OK) {
        // Flash LED to indicate error but don't halt
        led.setColor(LED::RED);
        HAL_Delay(2000);
        led.off();
    }
}

/**
 * @brief Enter STOP mode for low power sleep
 */
static void enterStopMode(void)
{
    // Enable LPUART wakeup from STOP mode (sets UESM bit in CR1)
    HAL_UARTEx_EnableStopMode(&hlpuart1);

    // Enable clock request in STOP mode (UCESM bit in CR3)
    // This allows LPUART to request HSI clock when START bit is detected
    SET_BIT(hlpuart1.Instance->CR3, USART_CR3_UCESM);

    // Suspend SysTick to prevent wakeup
    HAL_SuspendTick();

    // Enter STOP mode with low power regulator
    // Wake on any interrupt (RTC wakeup or LPUART in our case)
    HAL_PWR_EnterSTOPMode(PWR_LOWPOWERREGULATOR_ON, PWR_STOPENTRY_WFI);
}

/**
 * @brief Re-initialize system after waking from STOP mode
 */
static void wakeupFromStopMode(void)
{
    // Disable LPUART STOP mode after waking
    HAL_UARTEx_DisableStopMode(&hlpuart1);

    // After waking from STOP, system clock is MSI at reset speed
    // Need to reconfigure to our desired clock
    SystemClock_Config();

    // Resume SysTick
    HAL_ResumeTick();

    // Re-enable UART RX interrupt if it was stopped
    HAL_UART_Receive_IT(&hlpuart1, &uartRxByte, 1);
}

/**
 * @brief Determine wake type from RTC time and schedule
 * Called by onStateChange callback when entering AWAITING_CTS state.
 * Sets wake type in state machine and applies appropriate boot delay.
 */
static void determineWakeType(void)
{
    // Clear dedup buffer for new boot cycle - HAL tick was suspended during STOP mode
    // so old sequence numbers may still appear "recent"
    protocol.clearDedupBuffer();

    // Reset CTS flag for this wake cycle
    protocol.clearCtsReceived();

    // Get current RTC time and date
    RTC_TimeTypeDef time;
    RTC_DateTypeDef date;
    HAL_RTC_GetTime(&hrtc, &time, RTC_FORMAT_BIN);
    HAL_RTC_GetDate(&hrtc, &date, RTC_FORMAT_BIN);

    // Check if this is a scheduled event
    const PMU::ScheduleEntry *entry =
        protocol.getNextScheduledEntry(date.WeekDay, time.Hours, time.Minutes);

    // Check if we're within the wake interval window of a scheduled event
    // This handles the case where RTC doesn't wake exactly at the scheduled time
    uint32_t wakeIntervalMinutes = protocol.getWakeInterval() / 60;

    if (entry &&
        entry->isWithinWindow(date.WeekDay, time.Hours, time.Minutes, wakeIntervalMinutes)) {
        // This is a scheduled watering event
        pmuState.setWakeType(WakeType::SCHEDULED, entry);
    } else {
        // Normal periodic wake
        pmuState.setWakeType(WakeType::PERIODIC, nullptr);
    }

    // Small delay for RP2040 boot (scheduled needs more time)
    HAL_Delay(pmuState.wakeType() == WakeType::SCHEDULED ? 100 : 50);
}

/**
 * @brief RTC wakeup timer callback
 * This is called when the RTC wakeup interrupt fires
 * Keep this minimal to avoid stack overflow!
 */
void HAL_RTCEx_WakeUpTimerEventCallback(RTC_HandleTypeDef *hrtc)
{
    // Just set a flag - handle the actual work in main loop
    rtcWakeupFlag = true;
}

/**
 * @brief UART receive complete callback
 * Called when a byte is received via UART
 */
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == LPUART1) {
        // Process received byte through protocol
        protocol.processReceivedByte(uartRxByte);

        // Re-enable interrupt for next byte
        HAL_UART_Receive_IT(&hlpuart1, &uartRxByte, 1);
    }
}

/**
 * @brief UART send callback for protocol
 */
static void uartSendCallback(const uint8_t *data, uint8_t length)
{
    HAL_UART_Transmit(&hlpuart1, (uint8_t *)data, length, 1000);
}

/**
 * @brief Set wake interval callback
 */
static void setWakeIntervalCallback(uint32_t seconds)
{
    // Reconfigure RTC wakeup timer with new interval
    // (configureRTCWakeup handles deactivation internally)
    configureRTCWakeup(seconds);
}

/**
 * @brief Keep awake callback
 * @param seconds Number of seconds to stay awake
 */
static void keepAwakeCallback(uint16_t seconds)
{
    // TODO: Implement keep-awake timer
    // For now, we could just delay, but better to use a timer
    // This prevents entering sleep mode for the specified duration
}

/**
 * @brief Get tick callback for protocol deduplication
 */
static uint32_t getTickCallback()
{
    return HAL_GetTick();
}

/**
 * @brief Ready for sleep callback - RP2040 signals work complete
 */
static void readyForSleepCallback()
{
    // RP2040 is done with its work - dispatch READY_FOR_SLEEP event
    // State machine will transition to SLEEPING and handle DC/DC disable
    pmuState.dispatch(PmuEvent::READY_FOR_SLEEP);
}

/**
 * @brief Send wake notification to RP2040 based on current wake type
 * Called after CTS is received or CTS timeout in AWAITING_CTS state
 */
static void sendWakeNotification()
{
    if (pmuState.wakeType() == WakeType::SCHEDULED) {
        const PMU::ScheduleEntry *entry = pmuState.getScheduleEntry();
        if (entry) {
            protocol.sendWakeNotificationWithSchedule(PMU::WakeReason::Scheduled, entry);
        } else {
            protocol.sendWakeNotification(PMU::WakeReason::Scheduled);
        }
    } else {
        protocol.sendWakeNotification(PMU::WakeReason::Periodic);
    }
}

/**
 * @brief Update LED based on current PMU state and wake type
 * @param state Current PMU state
 * @param wakeType Current wake type (for color selection)
 */
static void updateLedForState(PmuState state, WakeType wakeType)
{
    // LED blinking is done inline in the main loop during wake states
    // This function handles the blink pattern
    static uint32_t lastBlinkTime = 0;
    static bool ledOn = false;

    // Select color based on wake type
    LED::Color wakeColor = (wakeType == WakeType::SCHEDULED) ? LED::RED : LED::ORANGE;

    switch (state) {
        case PmuState::AWAITING_CTS: {
            // Fast blink (250ms cycle) while waiting for CTS
            uint32_t now = HAL_GetTick();
            if (now - lastBlinkTime >= 125) {
                lastBlinkTime = now;
                ledOn = !ledOn;
                if (ledOn) {
                    led.setColor(wakeColor);
                } else {
                    led.off();
                }
            }
            break;
        }

        case PmuState::WAKE_ACTIVE: {
            // Slow blink (1s cycle) while RP2040 is working
            uint32_t now = HAL_GetTick();
            if (now - lastBlinkTime >= 500) {
                lastBlinkTime = now;
                ledOn = !ledOn;
                if (ledOn) {
                    led.setColor(wakeColor);
                } else {
                    led.off();
                }
            }
            break;
        }

        case PmuState::ERROR:
            led.setColor(LED::RED);
            break;

        case PmuState::SLEEPING:
        default:
            led.off();
            break;
    }
}

/**
 * @brief State change callback for PMU state machine
 * @param newState The new state after transition
 */
static void onStateChange(PmuState newState)
{
    // Determine wake type when entering AWAITING_CTS
    // This checks RTC time and schedule to set the correct wake type
    if (newState == PmuState::AWAITING_CTS) {
        determineWakeType();
    }

    // Send wake notification when entering WAKE_ACTIVE
    // This centralizes the notification logic - no need to call it explicitly
    if (newState == PmuState::WAKE_ACTIVE) {
        // If we came directly from SLEEPING (CTS woke us), determine wake type first
        // This is safe to call even if already called in AWAITING_CTS
        determineWakeType();
        sendWakeNotification();
    }
}

/* USER CODE END 4 */

/**
 * @brief  This function is executed in case of error occurrence.
 * @retval None
 */
void Error_Handler(void)
{
    /* USER CODE BEGIN Error_Handler_Debug */
    /* User can add his own implementation to report the HAL error return state */
    __disable_irq();
    while (1) {}
    /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
 * @brief  Reports the name of the source file and the source line number
 *         where the assert_param error has occurred.
 * @param  file: pointer to the source file name
 * @param  line: assert_param error line source number
 * @retval None
 */
void assert_failed(uint8_t *file, uint32_t line)
{
    /* USER CODE BEGIN 6 */
    /* User can add his own implementation to report the file name and line number,
       ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
    /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
