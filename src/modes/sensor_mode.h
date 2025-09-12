#pragma once

#include "application_mode.h"

/**
 * @brief Sensor-only mode (stub for now)
 */
class SensorMode : public ApplicationMode {
public:
    using ApplicationMode::ApplicationMode;
    
protected:
    void onStart() override;
};