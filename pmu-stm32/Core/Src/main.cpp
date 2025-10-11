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
#include "led.h"
#include "dcdc.h"
#include "pmu_protocol.h"
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

/* USER CODE BEGIN PV */
LED led;
DCDC dcdc;

// UART receive buffer
static uint8_t uartRxByte;

// Active watering session tracking
static bool wateringActive = false;
static uint32_t wateringStartTime = 0;
static uint16_t wateringDuration = 0;

// Protocol callbacks
static void uartSendCallback(const uint8_t* data, uint8_t length);
static void setWakeIntervalCallback(uint32_t seconds);
static void keepAwakeCallback(uint16_t seconds);

// Protocol instance
static PMU::Protocol protocol(uartSendCallback, setWakeIntervalCallback, keepAwakeCallback);
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_LPUART1_UART_Init(void);
static void MX_RTC_Init(void);
/* USER CODE BEGIN PFP */
static void flicker(LED& led, LED::Color color, uint32_t duration_ms);
static void configureRTCWakeup(uint32_t seconds);
static void enterStopMode(void);
static void wakeupFromStopMode(void);
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

  /* USER CODE BEGIN 2 */
  led.init();
  led.off();

  // Initialize and enable DC/DC converter
  dcdc.init();
  dcdc.enable();

  // Boot indication - flicker green LED
  flicker(led, LED::GREEN, 1000);

  // Configure RTC to wake every 15 seconds
  configureRTCWakeup(15);

  // Start UART receive interrupt
  HAL_UART_Receive_IT(&hlpuart1, &uartRxByte, 1);

  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
    // Check if we're in an active watering session
    if (wateringActive) {
      // Calculate elapsed time in seconds
      uint32_t elapsed = (HAL_GetTick() - wateringStartTime) / 1000;

      // Add 5 second grace period for RP2040 to complete watering
      if (elapsed >= (wateringDuration + 5)) {
        // Watering duration complete + grace period elapsed
        // Send schedule complete notification
        protocol.sendScheduleComplete();

        // Small delay to ensure message is sent
        HAL_Delay(5000);

        // Disable DC/DC to power down RP2040
        dcdc.disable();

        // End watering session
        wateringActive = false;
        wateringStartTime = 0;
        wateringDuration = 0;
      } else {
        // Still watering - stay awake, blink RED to show active watering
        led.setColor(LED::RED);
        HAL_Delay(100);
        led.off();
        HAL_Delay(900);  // Check every second
      }
    } else {
      // Not watering - normal sleep/wake cycle
      // Quick LED pulse to show we're awake
      led.setColor(LED::GREEN);
      HAL_Delay(100);
      led.off();

      // Enter low power STOP mode
      // Will wake up on RTC interrupt
      enterStopMode();

      // We're back! RTC woke us up
      wakeupFromStopMode();
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
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_LSI|RCC_OSCILLATORTYPE_MSI;
  RCC_OscInitStruct.LSIState = RCC_LSI_ON;
  RCC_OscInitStruct.MSIState = RCC_MSI_ON;
  RCC_OscInitStruct.MSICalibrationValue = 0;
  RCC_OscInitStruct.MSIClockRange = RCC_MSIRANGE_5;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_NONE;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_MSI;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV1;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_0) != HAL_OK)
  {
    Error_Handler();
  }
  PeriphClkInit.PeriphClockSelection = RCC_PERIPHCLK_LPUART1|RCC_PERIPHCLK_RTC;
  PeriphClkInit.Lpuart1ClockSelection = RCC_LPUART1CLKSOURCE_PCLK1;
  PeriphClkInit.RTCClockSelection = RCC_RTCCLKSOURCE_LSI;
  if (HAL_RCCEx_PeriphCLKConfig(&PeriphClkInit) != HAL_OK)
  {
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
  hlpuart1.Init.BaudRate = 9600;
  hlpuart1.Init.WordLength = UART_WORDLENGTH_8B;
  hlpuart1.Init.StopBits = UART_STOPBITS_1;
  hlpuart1.Init.Parity = UART_PARITY_NONE;
  hlpuart1.Init.Mode = UART_MODE_TX_RX;
  hlpuart1.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  hlpuart1.Init.OneBitSampling = UART_ONE_BIT_SAMPLE_DISABLE;
  hlpuart1.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;
  if (HAL_UART_Init(&hlpuart1) != HAL_OK)
  {
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
  hrtc.Init.AsynchPrediv = 127;
  hrtc.Init.SynchPrediv = 255;
  hrtc.Init.OutPut = RTC_OUTPUT_DISABLE;
  hrtc.Init.OutPutRemap = RTC_OUTPUT_REMAP_NONE;
  hrtc.Init.OutPutPolarity = RTC_OUTPUT_POLARITY_HIGH;
  hrtc.Init.OutPutType = RTC_OUTPUT_TYPE_OPENDRAIN;
  if (HAL_RTC_Init(&hrtc) != HAL_OK)
  {
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
  HAL_GPIO_WritePin(GPIOA, GPIO_PIN_1|GPIO_PIN_4|GPIO_PIN_5, GPIO_PIN_RESET);

  /*Configure GPIO pins : PA1 PA4 PA5 */
  GPIO_InitStruct.Pin = GPIO_PIN_1|GPIO_PIN_4|GPIO_PIN_5;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

  /* USER CODE BEGIN MX_GPIO_Init_2 */

  /* USER CODE END MX_GPIO_Init_2 */
}

/* USER CODE BEGIN 4 */

/**
 * @brief Flicker LED during boot for visual feedback
 */
static void flicker(LED& led, LED::Color color, uint32_t duration_ms) {
    const int flickers = duration_ms / 200;
    for (int i = 0; i < flickers; i++) {
        led.setColor(color);
        HAL_Delay(100);
        led.off();
        HAL_Delay(100);
    }
    led.off();
}

/**
 * @brief Configure RTC wakeup timer
 * @param seconds Number of seconds between wakeups
 */
static void configureRTCWakeup(uint32_t seconds) {
    // Disable wakeup timer first
    HAL_RTCEx_DeactivateWakeUpTimer(&hrtc);

    // Enable RTC interrupt in NVIC
    HAL_NVIC_SetPriority(RTC_IRQn, 0, 0);
    HAL_NVIC_EnableIRQ(RTC_IRQn);

    // LSI is typically 37kHz, with prescaler /16 = ~2.3kHz
    // To get 15 seconds: 15 * 2048 = 30720 (using RTCCLK_DIV16 gives ~2048 Hz)
    // For more accuracy, we'll use calculated value based on 37kHz LSI
    // With DIV16: 37000/16 = 2312.5 Hz, so for 15 sec: 15 * 2312.5 = 34687
    uint32_t wakeupCounter = seconds * 2048; // Approximate for LSI/16

    // Enable wakeup timer with interrupt
    if (HAL_RTCEx_SetWakeUpTimer_IT(&hrtc, wakeupCounter, RTC_WAKEUPCLOCK_RTCCLK_DIV16) != HAL_OK) {
        Error_Handler();
    }

    // Enable EXTI line 20 (RTC wakeup) for waking from STOP mode
    __HAL_RTC_WAKEUPTIMER_EXTI_ENABLE_IT();
    __HAL_RTC_WAKEUPTIMER_EXTI_ENABLE_RISING_EDGE();
}

/**
 * @brief Enter STOP mode for low power sleep
 */
static void enterStopMode(void) {
    // Suspend SysTick to prevent wakeup
    HAL_SuspendTick();

    // Enter STOP mode with low power regulator
    // Wake on any interrupt (RTC wakeup in our case)
    HAL_PWR_EnterSTOPMode(PWR_LOWPOWERREGULATOR_ON, PWR_STOPENTRY_WFI);
}

/**
 * @brief Re-initialize system after waking from STOP mode
 */
static void wakeupFromStopMode(void) {
    // After waking from STOP, system clock is MSI at reset speed
    // Need to reconfigure to our desired clock
    SystemClock_Config();

    // Resume SysTick
    HAL_ResumeTick();
}

/**
 * @brief RTC wakeup timer callback
 * This is called when the RTC wakeup interrupt fires
 */
void HAL_RTCEx_WakeUpTimerEventCallback(RTC_HandleTypeDef *hrtc)
{
    // Get current RTC time and date
    RTC_TimeTypeDef time;
    RTC_DateTypeDef date;
    HAL_RTC_GetTime(hrtc, &time, RTC_FORMAT_BIN);
    HAL_RTC_GetDate(hrtc, &date, RTC_FORMAT_BIN);

    // Check if this is a scheduled event
    const PMU::ScheduleEntry* entry = protocol.getNextScheduledEntry(
        date.WeekDay, time.Hours, time.Minutes);

    if (entry && entry->minutesUntil(date.WeekDay, time.Hours, time.Minutes) == 0) {
        // This is a scheduled watering event
        // Enable DC/DC to power up RP2040
        dcdc.enable();

        // Start watering session
        wateringActive = true;
        wateringStartTime = HAL_GetTick();
        wateringDuration = entry->duration;

        // Send wake notification
        protocol.sendWakeNotification(PMU::WakeReason::Scheduled);
    } else {
        // Normal periodic wake
        protocol.sendWakeNotification(PMU::WakeReason::Periodic);
    }
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
static void uartSendCallback(const uint8_t* data, uint8_t length)
{
    HAL_UART_Transmit(&hlpuart1, (uint8_t*)data, length, 1000);
}

/**
 * @brief Set wake interval callback
 */
static void setWakeIntervalCallback(uint32_t seconds)
{
    // Reconfigure RTC wakeup timer
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
  while (1)
  {
  }
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
