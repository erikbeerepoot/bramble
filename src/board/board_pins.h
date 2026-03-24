#pragma once

/**
 * @file board_pins.h
 * @brief Board pin configuration selector
 *
 * Includes the correct pin header based on BOARD_V4 compile definition.
 * Default is v3 (Adafruit Feather RP2040 RFM9x).
 */

#if defined(BOARD_V4)
#include "bramble_v4_pins.h"
#else
#include "bramble_v3_pins.h"
#endif
