#pragma once

/**
 * @file board_pins.h
 * @brief Board pin configuration selector
 *
 * Selects the pin header based on BOARD_V5 / BOARD_V4 compile definitions.
 * Default is v3 (Adafruit Feather RP2040 RFM9x).
 */

#if defined(BOARD_V5)
#include "bramble_v5_pins.h"
#elif defined(BOARD_V4)
#include "bramble_v4_pins.h"
#else
#include "bramble_v3_pins.h"
#endif
