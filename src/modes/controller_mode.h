#pragma once

#include "application_mode.h"

/**
 * @brief Controller mode with UI and scheduling (stub for now)
 */
class ControllerMode : public ApplicationMode {
public:
    using ApplicationMode::ApplicationMode;
    
protected:
    void onStart() override;
};