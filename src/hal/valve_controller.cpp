#include "valve_controller.h"

#include "hal/logger.h"

static Logger log("ValveCtrl");

// Initialize static constexpr array
constexpr uint8_t ValveController::VALVE_PINS[NUM_VALVES];

void ValveController::initialize()
{
    if (initialized_) {
        log.warn("Already initialized");
        return;
    }

    log.info("Initializing...");

    // Initialize H-bridge with new pin mapping
    // For FORWARD: HI_1 + LO_2 active (current flows 1->2)
    // For REVERSE: HI_2 + LO_1 active (current flows 2->1)
    hbridge_.initialize(PIN_MOTOR_HI_1, PIN_MOTOR_LO_1, PIN_MOTOR_HI_2, PIN_MOTOR_LO_2);

    // Initialize valve indexer
    indexer_.initialize(VALVE_PINS, NUM_VALVES);

    // Create DC latching drivers for all valves
    for (uint8_t i = 0; i < NUM_VALVES; i++) {
        drivers_[i] = std::make_unique<DCLatchingDriver>(&hbridge_);
        valve_states_[i] = ValveState::UNKNOWN;
    }

    initialized_ = true;

    // Close all valves to ensure known state
    closeAllValves();

    log.info("Initialized with %d valves", NUM_VALVES);
}

void ValveController::openValve(uint8_t valve_id)
{
    ensureInitialized();

    if (!isValidValveId(valve_id)) {
        log.error("Invalid valve ID %d", valve_id);
        return;
    }

    // Skip if already open
    if (valve_states_[valve_id] == ValveState::OPEN) {
        log.debug("Valve %d already open", valve_id);
        return;
    }

    operateValve(valve_id, true);
}

void ValveController::closeValve(uint8_t valve_id)
{
    ensureInitialized();

    if (!isValidValveId(valve_id)) {
        log.error("Invalid valve ID %d", valve_id);
        return;
    }

    // Skip if already closed
    if (valve_states_[valve_id] == ValveState::CLOSED) {
        log.debug("Valve %d already closed", valve_id);
        return;
    }

    operateValve(valve_id, false);
}

void ValveController::closeAllValves()
{
    ensureInitialized();

    log.info("Closing all valves...");

    for (uint8_t i = 0; i < NUM_VALVES; i++) {
        if (valve_states_[i] != ValveState::CLOSED) {
            closeValve(i);
        }
    }
}

ValveState ValveController::getValveState(uint8_t valve_id) const
{
    if (!isValidValveId(valve_id)) {
        return ValveState::UNKNOWN;
    }

    return valve_states_[valve_id];
}

uint8_t ValveController::getActiveValveMask() const
{
    uint8_t mask = 0;

    for (uint8_t i = 0; i < NUM_VALVES; i++) {
        if (valve_states_[i] == ValveState::OPEN) {
            mask |= (1 << i);
        }
    }

    return mask;
}

void ValveController::setValveType(uint8_t valve_id, ValveType type)
{
    ensureInitialized();

    if (!isValidValveId(valve_id)) {
        log.error("Invalid valve ID %d", valve_id);
        return;
    }

    // For now, only DC latching is supported
    if (type != ValveType::DC_LATCHING) {
        log.warn("Only DC_LATCHING valves supported currently");
    }
}

void ValveController::ensureInitialized() const
{
    if (!initialized_) {
        log.error("Not initialized! Call initialize() first");
    }
}

void ValveController::operateValve(uint8_t valve_id, bool open)
{
    log.info("Operating valve %d: %s", valve_id, open ? "OPEN" : "CLOSE");

    // First ensure ALL valves are deselected to prevent crosstalk
    indexer_.deselectAll();
    sleep_us(500);  // Give time for any gate charge to dissipate

    // Select the valve
    indexer_.selectValve(valve_id);

    // Longer delay to ensure MOSFET is fully on and stable
    sleep_us(1000);  // 1ms for gate to fully charge

    // Operate the valve through its driver
    if (open) {
        drivers_[valve_id]->open();
        valve_states_[valve_id] = ValveState::OPEN;
    } else {
        drivers_[valve_id]->close();
        valve_states_[valve_id] = ValveState::CLOSED;
    }

    // Small delay before deselecting to ensure operation completes
    sleep_us(500);

    // Deselect all valves for safety
    indexer_.deselectAll();
}