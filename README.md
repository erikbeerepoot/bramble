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
├── bramble.cpp                 # Main application
├── CMakeLists.txt             # Build configuration
├── CLAUDE.md                  # AI assistant instructions
├── PLAN.md                    # Development roadmap
└── src/
    ├── lora/                  # LoRa communication stack
    │   ├── sx1276.h/cpp      # LoRa radio driver
    │   ├── message.h/cpp     # Message protocol
    │   ├── reliable_messenger.h/cpp  # Reliable delivery
    │   ├── address_manager.h/cpp     # Network management
    │   └── hub_router.h/cpp   # Message routing
    ├── hal/                   # Hardware abstraction
    │   ├── flash.h/cpp       # QSPI flash storage
    │   ├── neopixel.h/cpp    # Status LED driver
    │   └── ws2812.pio        # PIO program for WS2812
    ├── config/                # Configuration management
    │   └── node_config.h/cpp # Persistent settings
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

### Build Commands
```bash
# Configure build
cmake -B build

# Build production version
cmake --build build

# Build test version
cmake -B build -DBUILD_TESTS=ON
cmake --build build

# Clean build
rm -rf build && cmake -B build && cmake --build build
```

### Flashing
1. Hold BOOTSEL button and connect Pico via USB
2. Copy `build/bramble.uf2` to the mounted drive
3. Pico will reboot and start running Bramble

## Configuration

### Node vs Hub Mode
Set in `bramble.cpp`:
```cpp
#define NODE_ADDRESS    ADDRESS_HUB      // Hub mode
#define NODE_ADDRESS    0x0001           // Node mode
```

### Demo vs Production Mode
```cpp
#define DEMO_MODE       true    // Colorful LEDs, test messages
#define DEMO_MODE       false   // Production sensor monitoring
```

### Network Settings
```cpp
#define SENSOR_INTERVAL_MS      30000   // Sensor reading interval
#define HEARTBEAT_INTERVAL_MS   60000   // Heartbeat frequency
#define MAIN_LOOP_DELAY_MS      100     // Processing delay
```

## Usage Examples

### Basic Node Setup
```cpp
// Initialize hardware
SX1276 lora(SPI_PORT, PIN_CS, PIN_RST, PIN_DIO0);
ReliableMessenger messenger(&lora, node_address);

// Send sensor data
uint8_t temp_data[] = {0x18, 0x5C};  // 24.5°C
messenger.sendSensorData(HUB_ADDRESS, SENSOR_TEMPERATURE, 
                        temp_data, sizeof(temp_data), RELIABLE);

// Send heartbeat
messenger.sendHeartbeat(HUB_ADDRESS, uptime_seconds, battery_level,
                       signal_strength, active_sensors, error_flags);
```

### Hub Operation
```cpp
// Hub automatically handles:
// - Node registration and address assignment
// - Message routing between nodes
// - Network monitoring and maintenance
// - Offline node detection
```

## LED Status Indicators

- **Hub**: Blue breathing pattern
- **Demo Node**: Rainbow color cycling
- **Production Node**: Green heartbeat pulse
- **Registration**: Flashing patterns during network join

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
- LoRa communication driver
- Message protocol and reliable delivery
- Node registration and address management
- Hub routing and network management
- Heartbeat monitoring and discovery
- Flash storage and configuration
- Comprehensive testing framework

### 🚧 Next Phase (Immediate)
- Error handling improvements
- Input validation and security
- Runtime configuration system
- SPI communication robustness
- Interrupt-driven operations

### 🔮 Later Phase (Future)
- Real sensor integration (DS18B20, SHT30, soil sensors)
- Actuator control (relays, servo valves)
- Power management and sleep modes
- Encryption and authentication
- Web interface and monitoring dashboard

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