/**
 ******************************************************************************
 * @file           : dcdc.cpp
 * @brief          : DC/DC converter control class implementation
 ******************************************************************************
 */

#include "dcdc.h"

DCDC::DCDC() : enabled(false) {
    // Constructor
}

void DCDC::init() {
    // Start with converter disabled
    disable();
}

void DCDC::enable() {
    HAL_GPIO_WritePin(GPIOA, DCDC_EN_PIN, GPIO_PIN_SET);
    enabled = true;
}

void DCDC::disable() {
    HAL_GPIO_WritePin(GPIOA, DCDC_EN_PIN, GPIO_PIN_RESET);
    enabled = false;
}

bool DCDC::isEnabled() const {
    return enabled;
}
