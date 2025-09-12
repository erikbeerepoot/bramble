#include "valve_indexer.h"
#include "hardware/gpio.h"
#include <cstdio>
#include <cstring>

void ValveIndexer::initialize(const uint8_t* valve_pins, uint8_t valve_count) {
    if (valve_count > MAX_VALVES) {
        printf("ERROR: Valve count %d exceeds maximum %d\n", valve_count, MAX_VALVES);
        valve_count = MAX_VALVES;
    }
    
    // Store configuration
    valve_count_ = valve_count;
    memcpy(valve_pins_, valve_pins, valve_count * sizeof(uint8_t));
    
    // Thread safety not needed for single-threaded operation
    
    // Configure all valve pins as outputs, initially OFF
    for (uint8_t i = 0; i < valve_count_; i++) {
        configurePin(valve_pins_[i]);
        printf("Valve %d configured on GPIO %d\n", i, valve_pins_[i]);
    }
    
    // No valve selected initially
    selected_valve_ = NO_VALVE_SELECTED;
    initialized_ = true;
    
    printf("ValveIndexer initialized with %d valves\n", valve_count_);
}

void ValveIndexer::selectValve(uint8_t valve_id) {
    if (!initialized_) {
        printf("ERROR: ValveIndexer not initialized\n");
        return;
    }
    
    if (!isValidValveId(valve_id)) {
        printf("ERROR: Invalid valve ID %d (max %d)\n", valve_id, valve_count_ - 1);
        return;
    }
    
    // First deselect any currently selected valve
    if (selected_valve_ != NO_VALVE_SELECTED) {
        gpio_put(valve_pins_[selected_valve_], 0);
    }
    
    // Select the new valve
    gpio_put(valve_pins_[valve_id], 1);
    selected_valve_ = valve_id;
    
    printf("Valve %d selected\n", valve_id);
}

void ValveIndexer::deselectAll() {
    if (!initialized_) {
        return;
    }
    
    // Turn off all valve MOSFETs
    for (uint8_t i = 0; i < valve_count_; i++) {
        gpio_put(valve_pins_[i], 0);
    }
    
    selected_valve_ = NO_VALVE_SELECTED;
}

void ValveIndexer::configurePin(uint8_t pin) {
    gpio_init(pin);
    gpio_set_dir(pin, GPIO_OUT);
    gpio_put(pin, 0);  // Start in OFF state
}