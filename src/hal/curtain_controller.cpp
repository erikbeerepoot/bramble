#include "curtain_controller.h"

#include "logger.h"

static Logger logger("Curtain");

void CurtainController::initialize(uint8_t open_pin, uint8_t close_pin)
{
    open_pin_ = open_pin;
    close_pin_ = close_pin;

    gpio_init(open_pin_);
    gpio_set_dir(open_pin_, GPIO_OUT);
    gpio_put(open_pin_, 0);

    gpio_init(close_pin_);
    gpio_set_dir(close_pin_, GPIO_OUT);
    gpio_put(close_pin_, 0);

    initialized_ = true;
    state_ = CurtainState::UNKNOWN;

    logger.info("Initialized (open=%d, close=%d)", open_pin_, close_pin_);
}

void CurtainController::open()
{
    if (!initialized_) {
        logger.error("Not initialized");
        return;
    }

    if (state_ == CurtainState::OPEN) {
        logger.info("Already open");
        return;
    }

    // Ensure close pin is LOW before driving open
    gpio_put(close_pin_, 0);
    gpio_put(open_pin_, 1);

    motor_start_time_ = to_ms_since_boot(get_absolute_time());
    state_ = CurtainState::OPENING;

    logger.info("Opening curtain");
}

void CurtainController::close()
{
    if (!initialized_) {
        logger.error("Not initialized");
        return;
    }

    if (state_ == CurtainState::CLOSED) {
        logger.info("Already closed");
        return;
    }

    // Ensure open pin is LOW before driving close
    gpio_put(open_pin_, 0);
    gpio_put(close_pin_, 1);

    motor_start_time_ = to_ms_since_boot(get_absolute_time());
    state_ = CurtainState::CLOSING;

    logger.info("Closing curtain");
}

void CurtainController::stop()
{
    if (!initialized_) {
        return;
    }

    if (!isMotorRunning()) {
        logger.info("Motor not running");
        return;
    }

    // During calibration OPENING phase, record the elapsed time as travel time
    if (state_ == CurtainState::CALIBRATING &&
        calibration_phase_ == CalibrationPhase::OPENING) {
        uint32_t measured = elapsed();
        travel_time_ms_ = measured;
        estimated_position_ = 1.0f;
        calibration_phase_ = CalibrationPhase::NONE;
        calibration_completed_ = true;
        stopMotor();
        state_ = CurtainState::OPEN;
        logger.info("Calibration complete: travel time = %lu ms", travel_time_ms_);
        return;
    }

    updateEstimatedPosition();
    stopMotor();
    calibration_phase_ = CalibrationPhase::NONE;
    state_ = CurtainState::STOPPED;

    logger.info("Stopped at estimated position %.0f%%", estimated_position_ * 100.0f);
}

void CurtainController::startCalibration()
{
    if (!initialized_) {
        logger.error("Not initialized");
        return;
    }

    logger.info("Starting calibration — closing curtain fully");

    // Drive curtain closed first
    gpio_put(open_pin_, 0);
    gpio_put(close_pin_, 1);

    motor_start_time_ = to_ms_since_boot(get_absolute_time());
    state_ = CurtainState::CALIBRATING;
    calibration_phase_ = CalibrationPhase::CLOSING;
    calibration_completed_ = false;
}

bool CurtainController::calibrationJustCompleted()
{
    if (calibration_completed_) {
        calibration_completed_ = false;
        return true;
    }
    return false;
}

void CurtainController::update()
{
    if (!isMotorRunning()) {
        return;
    }

    uint32_t elapsed_ms = elapsed();

    // Calibration state machine
    if (state_ == CurtainState::CALIBRATING) {
        switch (calibration_phase_) {
            case CalibrationPhase::CLOSING:
                // Run close for max_motor_run_ms to ensure fully closed
                if (elapsed_ms >= max_motor_run_ms_) {
                    stopMotor();
                    calibration_phase_ = CalibrationPhase::PAUSE;
                    calibration_pause_start_ = to_ms_since_boot(get_absolute_time());
                    logger.info("Calibration: fully closed, pausing");
                }
                return;

            case CalibrationPhase::PAUSE:
                // Brief pause before opening
                if (to_ms_since_boot(get_absolute_time()) - calibration_pause_start_ >= 1000) {
                    // Start opening — user will click Stop when fully open
                    gpio_put(close_pin_, 0);
                    gpio_put(open_pin_, 1);
                    motor_start_time_ = to_ms_since_boot(get_absolute_time());
                    calibration_phase_ = CalibrationPhase::OPENING;
                    logger.info("Calibration: opening — stop when fully open");
                }
                return;

            case CalibrationPhase::OPENING:
                // Safety timeout during calibration opening
                if (elapsed_ms >= max_motor_run_ms_) {
                    logger.error("Calibration safety timeout — stopping");
                    stopMotor();
                    calibration_phase_ = CalibrationPhase::NONE;
                    state_ = CurtainState::ERROR;
                }
                return;

            case CalibrationPhase::NONE:
                return;
        }
        return;
    }

    // Safety timeout — always enforced
    if (elapsed_ms >= max_motor_run_ms_) {
        logger.error("Safety timeout (%lums) — stopping motor", max_motor_run_ms_);
        stopMotor();
        state_ = CurtainState::ERROR;
        return;
    }

    // Travel complete check (only if calibrated)
    if (travel_time_ms_ > 0 && elapsed_ms >= travel_time_ms_) {
        if (state_ == CurtainState::OPENING) {
            stopMotor();
            estimated_position_ = 1.0f;
            state_ = CurtainState::OPEN;
            logger.info("Fully open (travel time %lums)", travel_time_ms_);
        } else if (state_ == CurtainState::CLOSING) {
            stopMotor();
            estimated_position_ = 0.0f;
            state_ = CurtainState::CLOSED;
            logger.info("Fully closed (travel time %lums)", travel_time_ms_);
        }
    }
}

void CurtainController::stopMotor()
{
    gpio_put(open_pin_, 0);
    gpio_put(close_pin_, 0);
}

void CurtainController::updateEstimatedPosition()
{
    if (travel_time_ms_ == 0) {
        // Not calibrated — can't estimate
        estimated_position_ = 0.5f;
        return;
    }

    uint32_t elapsed_ms = elapsed();
    float travel_fraction = static_cast<float>(elapsed_ms) / static_cast<float>(travel_time_ms_);
    if (travel_fraction > 1.0f) {
        travel_fraction = 1.0f;
    }

    if (state_ == CurtainState::OPENING) {
        estimated_position_ += travel_fraction * (1.0f - estimated_position_);
    } else if (state_ == CurtainState::CLOSING) {
        estimated_position_ -= travel_fraction * estimated_position_;
    }

    // Clamp
    if (estimated_position_ < 0.0f) {
        estimated_position_ = 0.0f;
    }
    if (estimated_position_ > 1.0f) {
        estimated_position_ = 1.0f;
    }
}

uint32_t CurtainController::elapsed() const
{
    return to_ms_since_boot(get_absolute_time()) - motor_start_time_;
}

const char *CurtainController::stateName(CurtainState state)
{
    switch (state) {
        case CurtainState::UNKNOWN:
            return "UNKNOWN";
        case CurtainState::OPEN:
            return "OPEN";
        case CurtainState::CLOSED:
            return "CLOSED";
        case CurtainState::OPENING:
            return "OPENING";
        case CurtainState::CLOSING:
            return "CLOSING";
        case CurtainState::STOPPED:
            return "STOPPED";
        case CurtainState::CALIBRATING:
            return "CALIBRATING";
        case CurtainState::ERROR:
            return "ERROR";
        default:
            return "UNKNOWN";
    }
}
