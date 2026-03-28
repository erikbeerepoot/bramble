#pragma once

#include <cstdint>

#include "pico/time.h"

#include "hardware/gpio.h"

/**
 * @brief Curtain motor states
 */
enum class CurtainState : uint8_t {
    UNKNOWN,      // Power-on, position unknown
    OPEN,         // Fully open
    CLOSED,       // Fully closed
    OPENING,      // Motor running in open direction
    CLOSING,      // Motor running in close direction
    STOPPED,      // Stopped mid-travel (partial position)
    CALIBRATING,  // Calibration in progress
    ERROR         // Safety timeout or fault
};

/**
 * @brief Internal phases of the calibration procedure
 */
enum class CalibrationPhase : uint8_t {
    NONE,     // Not calibrating
    CLOSING,  // Driving curtain fully closed
    PAUSE,    // Brief pause before opening
    OPENING   // Opening — user clicks Stop when fully open
};

/**
 * @brief Non-blocking motor controller for greenhouse roll-up curtains
 *
 * Drives a motor via two GPIO signals (relay-based motor reversing):
 *   - open_pin HIGH  → motor runs in open direction
 *   - close_pin HIGH → motor runs in close direction
 *   - both LOW       → motor stopped
 *
 * Motor runs for a calibrated travel time. The update() method must be called
 * from the main loop (~10ms) to check elapsed time and enforce safety limits.
 */
class CurtainController {
public:
    CurtainController() = default;

    /**
     * @brief Initialize GPIO pins for motor control
     * @param open_pin GPIO that drives the motor in the open direction
     * @param close_pin GPIO that drives the motor in the close direction
     */
    void initialize(uint8_t open_pin, uint8_t close_pin);

    /**
     * @brief Start opening the curtain
     *
     * Begins driving the motor in the open direction. Motor runs until
     * travel_time_ms elapses (checked in update()) or stop() is called.
     */
    void open();

    /**
     * @brief Start closing the curtain
     *
     * Begins driving the motor in the close direction. Motor runs until
     * travel_time_ms elapses (checked in update()) or stop() is called.
     */
    void close();

    /**
     * @brief Immediately stop the motor
     *
     * Cuts power to motor and updates estimated position based on elapsed time.
     */
    void stop();

    /**
     * @brief Non-blocking update — call from main loop
     *
     * Checks elapsed time against travel_time and safety timeout.
     * Stops the motor automatically when travel is complete or timeout hit.
     */
    void update();

    CurtainState getState() const { return state_; }

    /**
     * @brief Get estimated position (0.0 = closed, 1.0 = open)
     *
     * Approximate — computed from elapsed motor run time vs calibrated travel time.
     */
    float getEstimatedPosition() const { return estimated_position_; }

    uint32_t getTravelTime() const { return travel_time_ms_; }
    void setTravelTime(uint32_t travel_time_ms) { travel_time_ms_ = travel_time_ms; }

    bool isMotorRunning() const
    {
        return state_ == CurtainState::OPENING || state_ == CurtainState::CLOSING ||
               state_ == CurtainState::CALIBRATING;
    }

    /**
     * @brief Start interactive calibration procedure
     *
     * Drives curtain fully closed, pauses, then opens.
     * User must call stop() when curtain is fully open to record travel time.
     */
    void startCalibration();

    /**
     * @brief Check if calibration just completed (travel time was recorded)
     *
     * Returns true once after a calibration stop, then resets.
     */
    bool calibrationJustCompleted();

    /**
     * @brief Get human-readable state name
     */
    static const char *stateName(CurtainState state);

private:
    void stopMotor();
    void updateEstimatedPosition();
    uint32_t elapsed() const;

    uint8_t open_pin_ = 0;
    uint8_t close_pin_ = 0;
    bool initialized_ = false;

    CurtainState state_ = CurtainState::UNKNOWN;
    float estimated_position_ = 0.5f;  // Unknown → assume midpoint

    uint32_t travel_time_ms_ = 0;         // 0 = not calibrated
    uint32_t motor_start_time_ = 0;       // to_ms_since_boot timestamp
    uint32_t max_motor_run_ms_ = 180000;  // 3 minute safety timeout

    // Calibration state
    CalibrationPhase calibration_phase_ = CalibrationPhase::NONE;
    uint32_t calibration_pause_start_ = 0;
    bool calibration_completed_ = false;
};
