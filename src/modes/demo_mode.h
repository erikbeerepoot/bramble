#pragma once
#include "application_mode.h"

/**
 * @brief Demo mode for testing and development
 */
class DemoMode : public ApplicationMode {
private:
    
public:
    using ApplicationMode::ApplicationMode;
    
protected:
    void onStart() override;
};