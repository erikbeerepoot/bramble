#pragma once
#include "application_mode.h"

/**
 * @brief Production mode for actual sensor node operation
 */
class ProductionMode : public ApplicationMode {
private:
    
public:
    using ApplicationMode::ApplicationMode;
    
protected:
    void onStart() override;
};