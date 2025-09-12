#pragma once

#include "production_mode.h"

/**
 * @brief Generic mode - basically the old ProductionMode
 */
class GenericMode : public ProductionMode {
public:
    using ProductionMode::ProductionMode;
};