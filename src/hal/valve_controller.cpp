#include "valve_controller.h"
#include <cstdio>

// Initialize static constexpr array
constexpr uint8_t ValveController::VALVE_PINS[NUM_VALVES];

void ValveController::initialize() {
    if (initialized_) {
        printf("WARNING: ValveController already initialized\n");
        return;
    }
    
    printf("Initializing ValveController...\n");
    
    // Initialize H-bridge with new pin mapping
    // For FORWARD: HI_1 + LO_2 active (current flows 1->2)
    // For REVERSE: HI_2 + LO_1 active (current flows 2->1)
    hbridge_.initialize(PIN_MOTOR_HI_1, PIN_MOTOR_LO_1,
                       PIN_MOTOR_HI_2, PIN_MOTOR_LO_2);
    
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
    
    printf("ValveController initialized with %d valves\n", NUM_VALVES);
}

void ValveController::openValve(uint8_t valve_id) {
    ensureInitialized();
    
    if (!isValidValveId(valve_id)) {
        printf("ERROR: Invalid valve ID %d\n", valve_id);
        return;
    }
    
    // Skip if already open
    if (valve_states_[valve_id] == ValveState::OPEN) {
        printf("Valve %d already open\n", valve_id);
        return;
    }
    
    operateValve(valve_id, true);
}

void ValveController::closeValve(uint8_t valve_id) {
    ensureInitialized();
    
    if (!isValidValveId(valve_id)) {
        printf("ERROR: Invalid valve ID %d\n", valve_id);
        return;
    }
    
    // Skip if already closed
    if (valve_states_[valve_id] == ValveState::CLOSED) {
        printf("Valve %d already closed\n", valve_id);
        return;
    }
    
    operateValve(valve_id, false);
}

void ValveController::closeAllValves() {
    ensureInitialized();
    
    printf("Closing all valves...\n");
    
    for (uint8_t i = 0; i < NUM_VALVES; i++) {
        if (valve_states_[i] != ValveState::CLOSED) {
            closeValve(i);
        }
    }
}

ValveState ValveController::getValveState(uint8_t valve_id) const {
    if (!isValidValveId(valve_id)) {
        return ValveState::UNKNOWN;
    }
    
    return valve_states_[valve_id];
}

uint8_t ValveController::getActiveValveMask() const {
    uint8_t mask = 0;
    
    for (uint8_t i = 0; i < NUM_VALVES; i++) {
        if (valve_states_[i] == ValveState::OPEN) {
            mask |= (1 << i);
        }
    }
    
    return mask;
}

void ValveController::setValveType(uint8_t valve_id, ValveType type) {
    ensureInitialized();
    
    if (!isValidValveId(valve_id)) {
        printf("ERROR: Invalid valve ID %d\n", valve_id);
        return;
    }
    
    // For now, only DC latching is supported
    if (type != ValveType::DC_LATCHING) {
        printf("WARNING: Only DC_LATCHING valves supported currently\n");
    }
}

void ValveController::ensureInitialized() const {
    if (!initialized_) {
        printf("ERROR: ValveController not initialized! Call initialize() first\n");
    }
}

void ValveController::operateValve(uint8_t valve_id, bool open) {
    printf("Operating valve %d: %s\n", valve_id, open ? "OPEN" : "CLOSE");
    
    // Select the valve
    indexer_.selectValve(valve_id);
    
    // Small delay to ensure MOSFET is fully on
    sleep_us(100);
    
    // Operate the valve through its driver
    if (open) {
        drivers_[valve_id]->open();
        valve_states_[valve_id] = ValveState::OPEN;
    } else {
        drivers_[valve_id]->close();
        valve_states_[valve_id] = ValveState::CLOSED;
    }
    
    // Deselect all valves for safety
    indexer_.deselectAll();
}