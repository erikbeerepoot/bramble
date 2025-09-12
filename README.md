# Bramble - LoRa Farm Monitoring System

Bramble is a wireless sensor and actuator network designed for flower farms, built on the Raspberry Pi Pico platform with LoRa wireless communication. The system provides reliable, long-range communication between farm monitoring nodes and a central hub.

## Overview

Bramble implements a hub-and-spoke network architecture where:
- **Hub** manages the network, assigns addresses, and routes messages between nodes
- **Nodes** collect sensor data and control actuators (irrigation valves, pumps, fans)
- **LoRa wireless** provides long-range (up to several kilometers) communication
- **Reliable messaging** ensures critical commands reach their destination

## Hardware Platform

- **Board**: Adafruit Feather RP2040 LoRa
- **Microcontroller**: Raspberry Pi Pico (RP2040)
- **Radio**: SX1276 LoRa module (915 MHz)
- **Storage**: 8MB external QSPI flash
- **Indicator**: WS2812 NeoPixel LED
- **Power**: USB or battery operation

## Key Features

### 🌐 Network Management
- **Automatic node registration** with unique device ID assignment
- **Hub-based routing** for node-to-node communication
- **Heartbeat monitoring** with automatic offline detection
- **Network discovery** and active node tracking
- **Address management** with persistent storage

### 📡 Reliable Communication
- **Three delivery levels**: Best Effort, Reliable (ACK), Critical (persistent retry)
- **Message queuing** for offline nodes
- **CRC validation** and error detection
- **Sequence numbering** and duplicate detection

### 🔧 Farm Applications
- **Sensor monitoring**: Temperature, humidity, soil moisture, battery levels
- **Actuator control**: Irrigation valves, water pumps, ventilation fans
- **Node-to-node coordination**: Moisture sensors triggering irrigation
- **Status reporting**: Real-time network health and node status

### 🛠️ Developer Features
- **Comprehensive testing framework** with mock hardware
- **HAL abstraction** for hardware independence
- **Build configurations** for testing vs production
- **Extensive debugging** and status reporting

## Project Structure

```
bramble/
├── main.cpp                   # Unified entry point for all hardware variants
├── CMakeLists.txt             # Build configuration with hardware specialization
├── CLAUDE.md                  # AI assistant instructions
├── PLAN.md                    # Development roadmap
└── src/
    ├── lora/                  # LoRa communication stack
    │   ├── sx1276.h/cpp      # LoRa radio driver
    │   ├── message.h/cpp     # Message protocol
    │   ├── reliable_messenger.h/cpp  # Reliable delivery
    │   ├── address_manager.h/cpp     # Network management
    │   ├── hub_router.h/cpp   # Message routing
    │   └── network_stats.h/cpp       # Network statistics
    ├── modes/                 # Hardware-specific application modes
    │   ├── application_mode.h/cpp    # Base class for all modes
    │   ├── demo_mode.h/cpp          # Development/testing mode
    │   ├── hub_mode.h/cpp           # Generic hub functionality
    │   ├── irrigation_mode.h/cpp    # Irrigation node specialization
    │   ├── controller_mode.h/cpp    # Controller hub specialization
    │   ├── sensor_mode.h/cpp        # Sensor node specialization
    │   ├── production_mode.h/cpp    # Base production mode
    │   └── generic_mode.h/cpp       # Generic node functionality
    ├── hal/                   # Hardware abstraction
    │   ├── flash.h/cpp       # QSPI flash storage
    │   ├── neopixel.h/cpp    # Status LED driver
    │   ├── valve_controller.h/cpp    # Irrigation valve control
    │   ├── hbridge.h/cpp     # H-bridge motor driver
    │   ├── valve_indexer.h/cpp       # Individual valve control
    │   ├── logger.h/cpp      # Logging system
    │   ├── spi_device.h/cpp  # SPI communication helper
    │   └── ws2812.pio        # PIO program for WS2812
    ├── config/                # Configuration management
    │   ├── node_config.h/cpp # Node persistent settings
    │   ├── hub_config.h/cpp  # Hub registry management
    │   └── config_base.h/cpp # Base configuration class
    └── tests/                 # Testing framework
        ├── test_framework.h/cpp      # Test runner
        ├── mock_sx1276.h/cpp        # Mock hardware
        ├── reliability_tests.h/cpp   # Communication tests
        └── test_main.cpp            # Test entry point
```

## Message Protocol

Bramble uses a custom message protocol optimized for farm applications:

### Message Types
- **Sensor Data**: Temperature, humidity, soil moisture readings
- **Actuator Commands**: Valve control, pump operation
- **Heartbeat**: Node status and health monitoring
- **Registration**: New node network joining
- **ACK**: Message acknowledgment
- **Routing**: Node-to-node forwarding

### Delivery Guarantees
- **Best Effort**: Fire-and-forget (sensor readings)
- **Reliable**: ACK required with retries (actuator commands)
- **Critical**: Persistent retry until delivered (emergency shutoffs)

## Building and Flashing

### Prerequisites
- Raspberry Pi Pico SDK
- CMake 3.13+
- ARM GCC toolchain

### Hardware Variants
Bramble supports specialized builds for different hardware configurations:

- **IRRIGATION**: Valve control nodes with H-bridge and valve indexer
- **CONTROLLER**: Hub nodes with full network management
- **SENSOR**: Sensor-only nodes for monitoring
- **GENERIC**: General-purpose nodes (default)

### Build Commands
```bash
# Build irrigation variant (default)
cmake -B build -DHARDWARE_VARIANT=IRRIGATION
cmake --build build

# Build controller variant (hub)  
cmake -B build -DHARDWARE_VARIANT=CONTROLLER
cmake --build build

# Build sensor variant
cmake -B build -DHARDWARE_VARIANT=SENSOR
cmake --build build

# Build with production mode (minimal output)
cmake -B build -DHARDWARE_VARIANT=IRRIGATION -DDEMO_MODE=OFF
cmake --build build

# Build test version
cmake -B build -DBUILD_TESTS=ON
cmake --build build

# Clean build
rm -rf build && cmake -B build && cmake --build build
```

### Generated Outputs
Each build creates specialized executables:
- `bramble_irrigation.uf2` - Irrigation nodes with valve control
- `bramble_controller.uf2` - Controller hubs with full management
- `bramble_sensor.uf2` - Sensor-only monitoring nodes
- `bramble_generic.uf2` - General-purpose nodes

### Flashing
1. Hold BOOTSEL button and connect Pico via USB
2. Copy the appropriate `.uf2` file to the mounted drive
3. Pico will reboot and start running Bramble

## Configuration

### Hardware-Specific Features

#### Irrigation Nodes (`HARDWARE_IRRIGATION`)
- **Default Role**: Node (registers with hub)
- **Features**: 2-valve control via H-bridge driver
- **Pin Mapping**: 
  - H-bridge: GPIO26-29 (motor control)
  - Valves: GPIO24, GPIO25 (valve selection)
- **LED**: Green heartbeat pattern

#### Controller Hubs (`HARDWARE_CONTROLLER`) 
- **Default Role**: Hub (manages network)
- **Features**: Full network management and routing
- **LED**: Blue breathing pattern

#### Sensor Nodes (`HARDWARE_SENSOR`)
- **Default Role**: Node (sensor data only)
- **Features**: Optimized for sensor monitoring
- **LED**: Green heartbeat pattern

### Build-Time Configuration
Configuration is set via CMake variables:

```bash
# Hardware variant selection
-DHARDWARE_VARIANT=IRRIGATION|CONTROLLER|SENSOR|GENERIC

# Mode selection  
-DDEMO_MODE=ON|OFF    # Development vs production mode

# Test build
-DBUILD_TESTS=ON|OFF  # Include test framework
```

### Runtime Behavior
- **Hub Detection**: Automatic based on hardware variant default
- **Address Assignment**: Hub uses 0x0000, nodes auto-register for assigned addresses
- **Network Discovery**: Automatic registration and heartbeat monitoring

## Usage Examples

### Irrigation Node Operation
```cpp
// Built automatically into irrigation variant
IrrigationMode irrigation(messenger, lora, led, nullptr, nullptr, &network_stats);

// Valve control via LoRa commands
// Hub sends: ACTUATOR_VALVE, CMD_TURN_ON, valve_id=0
// Node responds by opening valve 0 via H-bridge
```

### Controller Hub Setup
```cpp
// Built automatically into controller variant  
ControllerMode controller(messenger, lora, led, &address_manager, &hub_router, &network_stats);

// Automatically handles:
// - Node registration and address assignment
// - Message routing between irrigation nodes
// - Network monitoring and valve status
// - Irrigation scheduling and coordination
```

### Sensor Node Monitoring
```cpp
// Built automatically into sensor variant
SensorMode sensor(messenger, lora, led, nullptr, nullptr, &network_stats);

// Automatically sends sensor data:
// - Temperature and humidity readings
// - Battery levels and signal strength  
// - Heartbeat status to hub
```

## LED Status Indicators

### By Hardware Variant
- **Controller Hub**: Blue breathing pattern during normal operation
- **Irrigation Node**: Green heartbeat pulse every 60 seconds
- **Sensor Node**: Green heartbeat pulse during data transmission
- **Generic Node**: Green heartbeat pulse

### By Mode
- **Demo Mode**: Rainbow color cycling and verbose output
- **Production Mode**: Simple heartbeat patterns, minimal output
- **Registration**: Role-based color during network join (blue=hub, green=node)
- **Error State**: Red solid color indicates hardware or network failure

## Network Topology

```
    [Soil Sensor]     [Weather Station]
          |                    |
          |                    |
    =====================================
                    [HUB]
    =====================================
          |                    |
          |                    |
    [Irrigation]          [Greenhouse]
     [Controller]           [Monitor]
```

- Hub manages all communication and routing
- Nodes can send messages to each other via hub
- Automatic failover and message queuing
- Real-time network status monitoring

## Development Status

### ✅ Completed Features
- LoRa communication driver (SX1276)
- Message protocol and reliable delivery
- Node registration and address management
- Hub routing and network management
- Heartbeat monitoring and discovery
- Flash storage and configuration persistence
- **Hardware specialization architecture**
- **Irrigation valve control system**
- **H-bridge driver for DC solenoids**
- **Build system for multiple hardware variants**
- Comprehensive testing framework

### 🚧 Next Phase (Immediate)  
- **Controller mode implementation** (scheduling, coordination)
- **Sensor mode implementation** (temperature, humidity, soil moisture)
- Error handling improvements
- Input validation and security
- SPI communication robustness
- Interrupt-driven operations

### 🔮 Later Phase (Future)
- Real sensor integration (DS18B20, SHT30, soil sensors)
- Advanced irrigation scheduling algorithms
- Power management and sleep modes
- Current monitoring for valve diagnostics
- Encryption and authentication
- Web interface and monitoring dashboard
- Multi-hop mesh networking

## Testing

### Unit Tests
```bash
# Build and run tests
cmake -B build -DBUILD_TESTS=ON
cmake --build build
# Run tests on hardware or in emulator
```

### Integration Tests
- Mock LoRa radio with controllable packet loss
- Reliability mechanism validation
- Registration protocol testing
- Network discovery simulation

## Troubleshooting

### Common Issues
1. **No LoRa Communication**: Check SPI wiring and antenna connection
2. **Registration Failures**: Verify hub is running and in range
3. **Flash Errors**: Check for corruption, try clearing configuration
4. **Build Errors**: Ensure Pico SDK is properly installed

### Debug Output
Enable via USB serial (115200 baud) for detailed logging:
- Message transmission/reception
- Network events and errors
- Node registration process
- Routing decisions and statistics

## Contributing

This project follows:
- KISS (Keep It Simple, Stupid) principle
- SOLID design principles
- Full word naming (no abbreviations: `address` not `addr`)
- Proper HAL abstraction for hardware independence

See `CLAUDE.md` for detailed development guidelines and `PLAN.md` for the current roadmap.

## License

This project is designed for agricultural use and educational purposes. See individual component licenses for third-party dependencies.

---

**Bramble**: Growing better farms through reliable wireless communication 🌿📡