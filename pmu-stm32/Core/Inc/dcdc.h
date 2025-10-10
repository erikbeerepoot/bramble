/**
 ******************************************************************************
 * @file           : dcdc.h
 * @brief          : DC/DC converter control class
 ******************************************************************************
 */

#ifndef DCDC_H
#define DCDC_H

#include "main.h"

/**
 * @brief DC/DC converter controller class
 *
 * Controls the enable pin for the DC/DC converter
 * Active HIGH: HIGH = converter enabled, LOW = converter disabled
 */
class DCDC {
public:
    /**
     * @brief Construct DCDC controller
     */
    DCDC();

    /**
     * @brief Initialize the DC/DC converter hardware
     * @note Should be called after GPIO initialization
     */
    void init();

    /**
     * @brief Enable the DC/DC converter
     */
    void enable();

    /**
     * @brief Disable the DC/DC converter
     */
    void disable();

    /**
     * @brief Check if DC/DC converter is enabled
     * @return true if enabled, false if disabled
     */
    bool isEnabled() const;

private:
    bool enabled;
};

#endif /* DCDC_H */
