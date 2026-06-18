/**
 * @file valve_test.cpp
 * @brief Standalone valve sweep test for irrigation hardware bring-up.
 *
 * Opens each valve in turn for 5 seconds, then closes it before moving on
 * to the next: valve 0 ON (5s) -> OFF, valve 1 ON (5s) -> OFF, ...
 * After the last valve it loops back to the first.
 *
 * Intended for probing the valve drive path with a DMM — no LoRa, PMU, or
 * hub registration so it never blocks on the bench.
 *
 * DC build:  cmake -B build -DHARDWARE_VARIANT=VALVETEST -DBOARD_VERSION=V4
 * AC build:  cmake -B build -DHARDWARE_VARIANT=VALVETEST_AC -DBOARD_VERSION=V5
 *
 * The AC build (HARDWARE_IRRIGATION_AC) drives one SSR gate per valve directly;
 * there is no H-bridge or indexer, but the open-5s-close sweep is identical.
 */
#include "pico/stdlib.h"

#include "hal/logger.h"
#include "hal/valve_controller.h"  // transitively provides Board:: pin map

namespace {
constexpr uint32_t VALVE_ON_DURATION_MS = 5000;
constexpr uint32_t INTER_VALVE_PAUSE_MS = 1000;
Logger logger("VALVETEST");
}  // namespace

int main()
{
    stdio_init_all();

    // Give USB/UART a moment to settle so the first logs are visible.
    sleep_ms(2000);

    logger.info("=== VALVE SWEEP TEST ===");
    logger.info("Sweeping %d valves, %lu ms each", ValveController::NUM_VALVES,
                static_cast<unsigned long>(VALVE_ON_DURATION_MS));

    // Print the full pin map so the bench observation can be correlated to silicon
    // GPIOs.
#ifdef HARDWARE_IRRIGATION_AC
    // AC: each valve id drives its own SSR gate GPIO high (open) / low (close).
    for (uint8_t i = 0; i < ValveController::NUM_VALVES; i++) {
        logger.info("  id %u -> SSR gate GPIO %u (high=open)", i, Board::VALVE_PINS[i]);
    }
#else
    // DC: each valve id selects one VALVE_PINS line; the shared H-bridge then
    // pulses FORWARD (open) or REVERSE (close).
    logger.info("H-bridge: HI_1=%u LO_1=%u HI_2=%u LO_2=%u (FORWARD=HI_1+LO_2, REVERSE=HI_2+LO_1)",
                Board::PIN_MOTOR_HI_1, Board::PIN_MOTOR_LO_1, Board::PIN_MOTOR_HI_2,
                Board::PIN_MOTOR_LO_2);
    for (uint8_t i = 0; i < ValveController::NUM_VALVES; i++) {
        logger.info("  id %u -> select GPIO %u", i, Board::VALVE_PINS[i]);
    }
#endif

    ValveController valve_controller;
    valve_controller.initialize();

    // Start from a known state — all valves closed.
    valve_controller.forceCloseAllValves();

    while (true) {
        for (uint8_t valve_id = 0; valve_id < ValveController::NUM_VALVES; valve_id++) {
            logger.info(">>> id %u (select GPIO %u) -> OPEN  — note which PHYSICAL valve moves",
                        valve_id, Board::VALVE_PINS[valve_id]);
            valve_controller.openValve(valve_id);

            sleep_ms(VALVE_ON_DURATION_MS);

            logger.info("<<< id %u (select GPIO %u) -> CLOSE", valve_id,
                        Board::VALVE_PINS[valve_id]);
            valve_controller.closeValve(valve_id);

            sleep_ms(INTER_VALVE_PAUSE_MS);
        }
        logger.info("Sweep complete — restarting");
    }
}
