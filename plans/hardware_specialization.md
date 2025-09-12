# Hardware Specialization Architecture

## Overview
This plan describes how to extend the Bramble platform to support specialized hardware variants while maintaining the flexibility for any node to act as a hub. The primary use cases are irrigation control nodes and a controller node that typically serves as the hub.

## Design Principles
1. **Role Flexibility**: Hub/node is a runtime role, not tied to hardware
2. **Hardware Specialization**: Different binaries for different hardware configurations
3. **Modularity**: Clean separation between network role and hardware capabilities
4. **Efficiency**: Only include code needed for specific hardware

## Architecture

### Build System
Create specialized executables based on hardware configuration:
```
bramble_controller.uf2  # UI, scheduling, defaults to hub role
bramble_irrigation.uf2  # Valve control + soil sensors, defaults to node role
bramble_sensor.uf2      # Temperature/humidity only, defaults to node role
bramble_generic.uf2     # Basic node, any sensors, defaults to node role
```

### Role Selection
Each binary has a build-time default role that makes sense for its hardware:
```cpp
// Set at compile time via CMake
#ifdef HARDWARE_CONTROLLER
    constexpr bool DEFAULT_IS_HUB = true;   // Controllers usually run as hub
#else
    constexpr bool DEFAULT_IS_HUB = false;  // Field devices usually nodes
#endif
```

## Implementation Plan

### 1. Refactor Mode Class Hierarchy
```
ApplicationMode (base)
├── NetworkRole (built into ApplicationMode)
│   ├── Hub functionality (if is_hub_)
│   └── Node functionality (if !is_hub_)
└── Derived Hardware Modes
    ├── ControllerMode   # UI, scheduling, commands
    ├── IrrigationMode   # Valves, soil sensors
    └── SensorMode       # Basic sensors only
```

### 2. CMake Build Configuration
```cmake
# Hardware variant selection
set(HARDWARE_VARIANT "GENERIC" CACHE STRING "Hardware variant type")
set_property(CACHE HARDWARE_VARIANT PROPERTY STRINGS 
    GENERIC CONTROLLER IRRIGATION SENSOR)

# Create specialized executables
if(HARDWARE_VARIANT STREQUAL "CONTROLLER")
    add_executable(bramble_controller
        main.cpp
        src/modes/controller_mode.cpp
        ${COMMON_SOURCES}
        ${HUB_SOURCES}  # Include hub capability
    )
    target_compile_definitions(bramble_controller PRIVATE
        HARDWARE_CONTROLLER=1
        DEFAULT_IS_HUB=1  # Controllers default to hub role
    )
elseif(HARDWARE_VARIANT STREQUAL "IRRIGATION")
    add_executable(bramble_irrigation
        main.cpp
        src/modes/irrigation_mode.cpp
        src/hal/valve_controller.cpp
        src/hal/soil_moisture.cpp
        ${COMMON_SOURCES}
        ${HUB_SOURCES}  # Include hub capability
    )
    target_compile_definitions(bramble_irrigation PRIVATE
        HARDWARE_IRRIGATION=1
        DEFAULT_IS_HUB=0  # Field devices default to node role
    )
endif()
```

### 3. Unified Main Entry Point
```cpp
// main.cpp - replaces bramble.cpp
int main() {
    stdio_init_all();
    
    // Use build-time default role
    constexpr bool is_hub = DEFAULT_IS_HUB;
    
    // Hardware initialization
    SX1276 lora(SPI_PORT, PIN_CS, PIN_RST, PIN_DIO0);
    NeoPixel led;
    
    if (!initializeHardware(lora, led)) {
        panic("Hardware initialization failed!");
    }
    
    // Show role with LED pattern
    if (is_hub) {
        led.set(0, 0, 255);  // Blue for hub
        printf("Starting as HUB\n");
    } else {
        led.set(0, 255, 0);  // Green for node
        printf("Starting as NODE\n");
    }
    
    // Create appropriate mode based on compile-time config
    #ifdef HARDWARE_CONTROLLER
        ControllerMode mode(messenger, lora, led, address_manager, 
                           hub_router, network_stats, is_hub);
    #elif HARDWARE_IRRIGATION
        IrrigationMode mode(messenger, lora, led, address_manager,
                           hub_router, network_stats, is_hub);
    #elif HARDWARE_SENSOR
        SensorMode mode(messenger, lora, led, address_manager,
                       hub_router, network_stats, is_hub);
    #else
        GenericMode mode(messenger, lora, led, address_manager,
                        hub_router, network_stats, is_hub);
    #endif
    
    mode.run();
    return 0;
}
```

### 4. Mode Implementations

#### Base ApplicationMode Changes
```cpp
class ApplicationMode {
protected:
    const bool is_hub_;  // Set at construction, never changes
    
public:
    ApplicationMode(ReliableMessenger& messenger, SX1276& lora,
                   NeoPixel& led, AddressManager* address_manager,
                   HubRouter* hub_router, NetworkStats* network_stats,
                   bool is_hub)
        : messenger_(messenger), lora_(lora), led_(led),
          address_manager_(address_manager), hub_router_(hub_router),
          network_stats_(network_stats), is_hub_(is_hub) {
        
        // Only initialize hub components if running as hub
        if (is_hub_ && !address_manager_) {
            address_manager_ = new AddressManager();
            hub_router_ = new HubRouter(messenger_, *address_manager_);
        }
    }
    
    void run() {
        // Common initialization
        onStart();
        
        // Role-specific initialization
        if (is_hub_) {
            printf("Initializing hub services...\n");
            address_manager_->initialize();
            // Hub doesn't need registration
        } else {
            printf("Initializing node services...\n");
            // Attempt registration with hub
            attemptRegistration();
        }
        
        // Main loop (same for both roles)
        while (true) {
            // ... existing main loop
        }
    }
};
```

#### ControllerMode
```cpp
class ControllerMode : public ApplicationMode {
private:
    // Controller-specific hardware
    UIManager ui_;
    ScheduleEngine scheduler_;
    ZoneManager zones_;
    
public:
    using ApplicationMode::ApplicationMode;  // Inherit constructor
    
    void onStart() override {
        // Set capabilities
        capabilities_ = CAP_CONTROLLER | CAP_SCHEDULING;
        node_type_ = NODE_TYPE_CONTROLLER;
        
        // Initialize UI (buttons, display)
        ui_.initialize();
        
        // Load schedules and zones
        scheduler_.loadSchedules();
        zones_.loadConfiguration();
        
        // LED pattern for controller
        if (is_hub_) {
            led_pattern_ = std::make_unique<PulsePattern>(led_, 0, 0, 255); // Blue pulse
        } else {
            // Unlikely but possible - controller as node
            led_pattern_ = std::make_unique<BlinkPattern>(led_, 255, 0, 255); // Purple blink
        }
        
        // Add periodic tasks
        task_manager_.addTask(
            [this](uint32_t time) { checkSchedules(); },
            1000,  // Check every second
            "Schedule Check"
        );
        
        task_manager_.addTask(
            [this](uint32_t time) { updateUI(); },
            100,  // Update UI every 100ms
            "UI Update"
        );
    }
    
    void handleUIEvent(UIEvent event) {
        switch (event.type) {
            case UI_BUTTON_PRESS:
                handleButtonPress(event.button_id);
                break;
            case UI_ENCODER_TURN:
                handleEncoderTurn(event.delta);
                break;
        }
    }
};
```

#### IrrigationMode
```cpp
class IrrigationMode : public ApplicationMode {
private:
    ValveController valve_controller_;
    SoilMoistureSensor soil_sensor_;
    WaterUsageTracker water_tracker_;
    
public:
    using ApplicationMode::ApplicationMode;
    
    void onStart() override {
        capabilities_ = CAP_VALVE_CONTROL | CAP_SOIL_MOISTURE;
        node_type_ = NODE_TYPE_HYBRID;
        
        // Initialize hardware
        valve_controller_.initialize();
        soil_sensor_.initialize();
        
        // LED pattern
        if (is_hub_) {
            // Emergency hub mode - alternating blue/green
            led_pattern_ = std::make_unique<AlternatePattern>(led_, 0, 255, 0, 0, 0, 255);
            logger_.warn("Irrigation node running as emergency hub!");
        } else {
            // Normal operation - green heartbeat
            led_pattern_ = std::make_unique<HeartbeatPattern>(led_, 0, 255, 0);
        }
        
        // Add sensor reading task
        task_manager_.addTask(
            [this](uint32_t time) { readAndReportSensors(); },
            30000,  // Every 30 seconds
            "Sensor Reading"
        );
    }
    
    void onActuatorCommand(const ActuatorPayload* payload) override {
        if (payload->actuator_type == ACTUATOR_VALVE) {
            uint8_t valve_id = payload->params[0];
            
            if (payload->command == CMD_TURN_ON) {
                valve_controller_.openValve(valve_id);
                water_tracker_.startFlow(valve_id);
            } else if (payload->command == CMD_TURN_OFF) {
                valve_controller_.closeValve(valve_id);
                auto usage = water_tracker_.stopFlow(valve_id);
                reportWaterUsage(valve_id, usage);
            }
        }
    }
};
```

### 5. Configuration Extensions

#### Controller Configuration
```cpp
class ControllerConfig : public ConfigBase {
    struct Zone {
        char name[32];
        uint16_t node_addresses[MAX_NODES_PER_ZONE];
        uint8_t valve_ids[MAX_VALVES_PER_ZONE];
        uint8_t node_count;
    };
    
    struct Schedule {
        uint8_t zone_id;
        uint32_t start_time;  // Seconds since midnight
        uint32_t duration_seconds;
        uint8_t days_of_week;  // Bitmask
        bool enabled;
    };
    
    Zone zones[MAX_ZONES];
    Schedule schedules[MAX_SCHEDULES];
    uint8_t zone_count;
    uint8_t schedule_count;
};
```

### 6. Directory Structure
```
src/
├── main.cpp                      # Unified entry point
├── modes/
│   ├── application_mode.h/cpp    # Base class with role support
│   ├── controller_mode.h/cpp     # Controller specialization
│   ├── irrigation_mode.h/cpp     # Irrigation specialization
│   ├── sensor_mode.h/cpp         # Sensor-only specialization
│   └── generic_mode.h/cpp        # Generic node
├── hal/
│   ├── valve_controller.h/cpp    # Existing valve control
│   ├── soil_moisture.h/cpp       # Soil sensor interface
│   ├── ui_manager.h/cpp          # Buttons, display, encoder
│   └── ...
├── services/
│   ├── schedule_engine.h/cpp     # Scheduling logic
│   ├── zone_manager.h/cpp        # Zone management
│   └── water_usage_tracker.h/cpp # Track water consumption
└── config/
    ├── controller_config.h/cpp    # Controller settings
    └── irrigation_config.h/cpp    # Irrigation settings
```

## Build Examples

```bash
# Build controller (defaults to hub)
cmake -B build -DHARDWARE_VARIANT=CONTROLLER
cmake --build build
# Output: bramble_controller.uf2

# Build irrigation node (defaults to node)
cmake -B build -DHARDWARE_VARIANT=IRRIGATION  
cmake --build build
# Output: bramble_irrigation.uf2

# Build basic sensor (defaults to node)
cmake -B build -DHARDWARE_VARIANT=SENSOR
cmake --build build  
# Output: bramble_sensor.uf2
```

## Deployment Example

```
Farm Network:
├── Greenhouse Controller      # bramble_controller.uf2 (running as hub)
│   ├── Display + Buttons     
│   ├── Schedules: 6AM, 6PM
│   └── 3 Zones configured
│
├── Irrigation Node 1         # bramble_irrigation.uf2 (running as node)
│   ├── Valves: 1, 2
│   ├── Soil Moisture: 45%
│   └── Address: 0x0001
│
├── Irrigation Node 2         # bramble_irrigation.uf2 (running as node)
│   ├── Valves: 1, 2  
│   ├── Soil Moisture: 38%
│   └── Address: 0x0002
│
└── Weather Station          # bramble_sensor.uf2 (running as node)
    ├── Temperature: 24°C
    ├── Humidity: 65%
    └── Address: 0x0003
```

## Future Extensions

1. **Configuration UI**: Add USB serial commands to change settings
2. **Role Override**: Button combination at boot to toggle role
3. **Failover**: Automatic hub election if primary hub fails
4. **OTA Updates**: Different firmware for different hardware types

This architecture provides specialized functionality while keeping role selection simple through build-time defaults.