# Valve Switcher Implementation Plan

## Overview
Implementation of irrigation valve control system supporting DC latching solenoids with future extensibility for AC valves. The system uses an H-bridge driver for bidirectional control and a valve indexer for selecting individual valves.

## Architecture

### Layered Design
1. **Low-level H-Bridge Control** - Direct MOSFET control with safety
2. **Valve Driver Abstraction** - Supports different valve types
3. **High-level Valve Controller** - Simple API for application use

### GPIO Pin Allocation
Based on current usage (SPI on 16-19, LoRa interrupt on others):
- **H-Bridge Control:**
  - HIGH_SIDE_A: GPIO 2
  - LOW_SIDE_A: GPIO 3  
  - HIGH_SIDE_B: GPIO 6
  - LOW_SIDE_B: GPIO 7
- **Valve Indexer:**
  - VALVE_1: GPIO 8
  - VALVE_2: GPIO 9
  - VALVE_3: GPIO 10
  - VALVE_4: GPIO 11

## Implementation Details

### Layer 1: H-Bridge Hardware Control
```cpp
class HBridge {
public:
    enum Direction { FORWARD, REVERSE };
    
    void initialize(uint8_t pin_high_a, uint8_t pin_low_a, 
                   uint8_t pin_high_b, uint8_t pin_low_b);
    void pulse(Direction dir, uint32_t duration_ms);
    void emergencyOff();
    
private:
    // GPIO pins for MOSFETs
    uint8_t pin_high_side_a_;
    uint8_t pin_low_side_a_;
    uint8_t pin_high_side_b_;
    uint8_t pin_low_side_b_;
    
    // Safety features
    static constexpr uint32_t MAX_PULSE_DURATION_MS = 300;
    void setAllPinsLow();
};
```

### Layer 2: Valve Driver Interface
```cpp
class ValveDriver {
public:
    virtual ~ValveDriver() = default;
    virtual void open() = 0;
    virtual void close() = 0;
    virtual bool requiresContinuousPower() = 0;
    virtual ValveType getType() = 0;
};

class DCLatchingDriver : public ValveDriver {
public:
    DCLatchingDriver(HBridge* hbridge, uint32_t pulse_duration_ms = 250);
    void open() override;
    void close() override;
    bool requiresContinuousPower() override { return false; }
    ValveType getType() override { return ValveType::DC_LATCHING; }
    
private:
    HBridge* hbridge_;
    uint32_t pulse_duration_ms_;
};
```

### Layer 3: Valve Indexer
```cpp
class ValveIndexer {
public:
    void initialize(const uint8_t* valve_pins, uint8_t valve_count);
    void selectValve(uint8_t valve_id);
    void deselectAll();
    uint8_t getSelectedValve() const { return selected_valve_; }
    
private:
    static constexpr uint8_t MAX_VALVES = 8;
    uint8_t valve_pins_[MAX_VALVES];
    uint8_t valve_count_;
    uint8_t selected_valve_;
};
```

### Layer 4: High-Level Valve Controller
```cpp
class ValveController {
public:
    static constexpr uint8_t NUM_VALVES = 4;
    
    void initialize();
    
    // High-level API
    void openValve(uint8_t valve_id);
    void closeValve(uint8_t valve_id);
    void closeAllValves();
    
    // State queries
    ValveState getValveState(uint8_t valve_id) const;
    uint8_t getActiveValveMask() const;
    
    // Configuration
    void setValveType(uint8_t valve_id, ValveType type);
    
private:
    HBridge hbridge_;
    ValveIndexer indexer_;
    std::unique_ptr<ValveDriver> drivers_[NUM_VALVES];
    ValveState valve_states_[NUM_VALVES];
    
    // Thread safety
    mutable critical_section_t valve_mutex_;
    
    // Safety
    void ensureInitialized();
    bool isValidValveId(uint8_t valve_id) const;
};
```

## Integration Points

### Message Protocol
The existing protocol already supports valve commands:
- Message Type: `MSG_TYPE_ACTUATOR_CMD`
- Actuator Type: `ACTUATOR_VALVE`
- Commands: `CMD_TURN_ON` (0x01) / `CMD_TURN_OFF` (0x00)
- Parameter: valve_id (0-3)

### Production Mode Integration
```cpp
// In ProductionMode class
void handleActuatorCommand(const uint8_t* payload, size_t length) {
    uint8_t actuator_type = payload[0];
    uint8_t command = payload[1];
    uint8_t valve_id = payload[3];  // First parameter
    
    if (actuator_type == ACTUATOR_VALVE) {
        if (command == CMD_TURN_ON) {
            valve_controller_.openValve(valve_id);
        } else if (command == CMD_TURN_OFF) {
            valve_controller_.closeValve(valve_id);
        }
    }
}
```

### Status Reporting
Include valve states in heartbeat messages:
```cpp
uint8_t active_valves = valve_controller_.getActiveValveMask();
// Include in heartbeat data
```

## Safety Features

1. **Mutex Protection** - Thread-safe valve operations
2. **State Tracking** - Prevent redundant operations
3. **Emergency Shutoff** - `closeAllValves()` for safety
4. **Pulse Duration Limits** - Maximum 300ms to prevent coil damage
5. **Initialization Checks** - Ensure proper setup before operation
6. **Default Safe State** - All valves closed on startup

## Future Extensibility

### AC Valve Support
The `ValveDriver` interface allows easy addition of AC valve support:
```cpp
class ACValveDriver : public ValveDriver {
    void open() override { relay_.energize(); }
    void close() override { relay_.deenergize(); }
    bool requiresContinuousPower() override { return true; }
};
```

### Current Monitoring
Stub for current monitoring in valve operations:
```cpp
// In ValveController
float getValveCurrent(uint8_t valve_id);  // Returns 0.0 for now
bool detectValveFault(uint8_t valve_id);  // Stub for future
```

## File Structure
```
src/hal/
  ├── valve_controller.h
  ├── valve_controller.cpp
  ├── hbridge.h
  ├── hbridge.cpp
  ├── valve_indexer.h
  └── valve_indexer.cpp
```

## Implementation Order
1. Create H-Bridge low-level control
2. Implement Valve Indexer
3. Create DC Latching Driver
4. Build high-level Valve Controller
5. Integrate with Production Mode
6. Add message handling
7. Update status reporting

## Testing Approach
Since tests are not requested, manual verification will include:
- LED indicators on valve pins during development
- Serial output for state changes
- Oscilloscope verification of pulse timing
- Multimeter testing of MOSFET switching