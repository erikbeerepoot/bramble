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
- **Storage**: MT25QL01GBBB 128MB external flash (sensor data), 8MB QSPI flash (config)
- **Indicator**: WS2812 NeoPixel LED
- **Power**: USB or battery operation

### Pin Configuration

#### LoRa Module (SX1276) - SPI1

| Signal | Pin | GPIO | Notes |
|--------|-----|------|-------|
| MISO | 11 | GPIO8 | SPI1 RX |
| SCK | 17 | GPIO14 | SPI1 SCK |
| MOSI | 18 | GPIO15 | SPI1 TX |
| CS | 27 | GPIO16 | Chip select |
| RST | 28 | GPIO17 | Reset |
| DIO0 | 32 | GPIO21 | Interrupt |

#### External Flash (MT25QL) - SPI1 (shared with LoRa)

| Signal | Pin | GPIO | Notes |
|--------|-----|------|-------|
| MISO | 11 | GPIO8 | Shared with LoRa |
| SCK | 17 | GPIO14 | Shared with LoRa |
| MOSI | 18 | GPIO15 | Shared with LoRa |
| CS | 8 | GPIO6 | Flash chip select |
| RST | 9 | GPIO7 | Flash reset |

#### Other Peripherals

| Signal | Pin | GPIO | Notes |
|--------|-----|------|-------|
| NeoPixel | 6 | GPIO4 | WS2812 data |
| UART TX | 2 | GPIO0 | Debug output |
| UART RX | 3 | GPIO1 | Debug input |

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

### 💾 Sensor Data Storage
- **128MB flash circular buffer**: ~10.6 million records capacity
- **12-year storage**: At 30-second sampling intervals
- **CRC16 validation**: Data integrity verification
- **Batch transmission**: Up to 20 records per LoRa message
- **Transmission tracking**: Records marked after successful ACK
- **Offline resilience**: Data preserved during network outages

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
    ├── storage/               # Sensor data persistence
    │   ├── sensor_flash_buffer.h/cpp  # 128MB circular buffer
    │   ├── sensor_flash_metadata.h    # Buffer state tracking
    │   └── sensor_data_record.h       # 12-byte record format
    └── tests/                 # Testing framework
        ├── test_framework.h/cpp      # Test runner
        ├── mock_sx1276.h/cpp        # Mock hardware
        ├── reliability_tests.h/cpp   # Communication tests
        └── test_main.cpp            # Test entry point
├── api/                       # Raspberry Pi REST API
│   ├── app.py                # Flask application
│   ├── database.py           # DuckDB sensor storage
│   ├── serial_interface.py   # Hub serial communication
│   └── config.py             # API configuration
├── dashboard/                 # React web dashboard
│   ├── src/
│   │   ├── components/       # React components
│   │   ├── api/client.ts     # API client
│   │   └── App.tsx           # Main application
│   ├── package.json          # Dependencies
│   └── vite.config.ts        # Vite configuration
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

### First-Time Setup
After cloning, run the setup script to configure git hooks:
```bash
./setup.sh
```

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

### Initialization
All nodes display **orange blinking** during startup while waiting for RTC synchronization via heartbeat response from the hub. Once RTC is synced, nodes transition to their operational patterns.

### Operational Patterns (by Hardware Variant)
- **Controller Hub**: Blue breathing pattern
- **Irrigation Node**: Green heartbeat pulse
- **Sensor Node**: Purple breathing pattern
- **Generic Node**: Green heartbeat pulse

### Special States
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

## Raspberry Pi Integration

The hub connects to a Raspberry Pi via serial (UART) for data storage and web access.

### System Architecture

```
[Sensor Nodes] --LoRa--> [Hub/Pico] --Serial--> [Raspberry Pi] --REST--> [Web/Apps]
                              |                       |
                         128MB Flash              DuckDB
                       (local buffer)          (permanent storage)
```

### Serial Protocol

The hub forwards sensor data to the Raspberry Pi using a text-based protocol:

```
# Single reading
SENSOR_DATA <node_addr> TEMP|HUM <value>

# Batch transmission (up to 20 records)
SENSOR_BATCH <node_addr> <count>
SENSOR_RECORD <node_addr> <timestamp> <temp> <humidity> <flags>
...
BATCH_COMPLETE <node_addr> <count>

# Acknowledgment (Pi to Hub)
BATCH_ACK <node_addr> <count> <status>

# Time sync (Hub queries, Pi responds)
GET_DATETIME
DATETIME YYYY-MM-DD HH:MM:SS <day_of_week>
```

### Database Schema

Sensor readings are stored in DuckDB with the following schema:

```sql
CREATE TABLE sensor_readings (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    node_address INTEGER NOT NULL,
    device_id UBIGINT,                    -- Hardware unique identifier
    timestamp INTEGER NOT NULL,           -- Unix timestamp
    temperature_centidegrees INTEGER,     -- Temperature in 0.01°C
    humidity_centipercent INTEGER,        -- Humidity in 0.01%
    flags INTEGER DEFAULT 0,
    received_at INTEGER NOT NULL,
    UNIQUE(node_address, timestamp)
);

CREATE TABLE nodes (
    address INTEGER PRIMARY KEY,
    device_id UBIGINT UNIQUE,
    node_type TEXT NOT NULL,
    first_seen_at INTEGER NOT NULL,
    last_seen_at INTEGER NOT NULL,
    total_readings INTEGER DEFAULT 0
);

CREATE TABLE node_metadata (
    address INTEGER PRIMARY KEY,
    name VARCHAR,                         -- Friendly name
    location VARCHAR,                     -- Location description
    notes VARCHAR,                        -- Additional notes
    zone_id INTEGER,                      -- FK to zones table
    updated_at INTEGER NOT NULL
);

CREATE TABLE zones (
    id INTEGER PRIMARY KEY,
    name VARCHAR NOT NULL,                -- Zone name (e.g., "Greenhouse")
    color VARCHAR(7) NOT NULL,            -- Hex color (e.g., "#4CAF50")
    description VARCHAR                   -- Optional description
);
```

### REST API

The Raspberry Pi runs a Flask-based REST API for querying sensor data.

#### Endpoints

| Method | Endpoint | Description |
|--------|----------|-------------|
| GET | `/api/nodes` | List all nodes with metadata |
| GET | `/api/nodes/<addr>/metadata` | Get node name/location/notes |
| PUT | `/api/nodes/<addr>/metadata` | Update node name/location/notes |
| GET | `/api/sensor-data` | Query sensor readings with filters |
| GET | `/api/sensor-data/export` | Export readings as CSV |
| GET | `/api/nodes/<addr>/sensor-data` | Get readings for specific node |
| GET | `/api/nodes/<addr>/latest` | Get most recent reading |
| GET | `/api/nodes/<addr>/statistics` | Get node statistics |
| GET | `/api/nodes/<addr>/error-history` | Get error flag history for a node (requires start/end params) |

#### Query Parameters

- `node` - Filter by node address
- `start` - Start timestamp (Unix)
- `end` - End timestamp (Unix)
- `limit` - Maximum records (default 1000)
- `offset` - Pagination offset

#### Examples

```bash
# Get latest 100 readings
curl http://pi:5000/api/sensor-data?limit=100

# Get readings from node 0x0001 in last 24 hours
curl "http://pi:5000/api/nodes/1/sensor-data?start=$(date -d '24 hours ago' +%s)"

# Export all data as CSV
curl http://pi:5000/api/sensor-data/export > sensor_data.csv

# Get statistics for node 0x0001
curl http://pi:5000/api/nodes/1/stats

# Get error history for node 0x0001 from last 7 days
curl "http://pi:5000/api/nodes/1/error-history?start=$(date -d '7 days ago' +%s)&end=$(date +%s)"
```

#### Response Format

```json
{
  "readings": [
    {
      "node_address": 1,
      "timestamp": 1704067200,
      "temperature_celsius": 22.5,
      "humidity_percent": 65.0,
      "flags": 0,
      "received_at": 1704067205
    }
  ],
  "count": 1,
  "total": 15000
}
```

### Running the API

```bash
# Create data directory for database (first time only)
sudo mkdir -p /data
sudo chown $USER:$USER /data

# Install and run
cd api
uv sync       # or: pip install -r requirements.txt
uv run app.py # or: python app.py
```

### Web Dashboard

The web dashboard is a React SPA that connects to the REST API to display sensor data and manage nodes.

#### Features
- **Node list view** with online/offline status indicators
- **Zone management** for organizing nodes into logical groups
  - Color-coded zone indicators on node cards
  - Collapsible zone sections in node list
  - Create zones with custom colors via modal
  - Assign/unassign nodes to zones
- **Node metadata editing** (friendly names, locations, notes, zone assignment)
- **Temperature/humidity charts** using Plotly.js
- **Time range selection** (1h, 6h, 24h, 7d, 30d)
- **Statistics display** (min/max/avg values)
- **Configurable API URL** (stored in browser localStorage)

#### Running the Dashboard

```bash
cd dashboard
npm install
npm run dev
```

The dashboard runs at `http://localhost:3000`. Configure the API URL in Settings to point to your Raspberry Pi (e.g., `http://pi:5000`).

#### Building for Production

```bash
cd dashboard
npm run build
```

The built files in `dist/` can be served by any static file server (nginx, Apache, etc.).

Configuration via environment variables:
- `SERIAL_PORT` - Serial port path (default: `/dev/ttyAMA0`)
- `SERIAL_BAUD` - Baud rate (default: `115200`)
- `SENSOR_DB_PATH` - Database file path (default: `/data/sensor_data.db`)
- `HOST` - API host (default: `0.0.0.0`)
- `PORT` - API port (default: `5000`)

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
- **128MB sensor data flash storage** with circular buffer
- **Batch LoRa transmission protocol** (SENSOR_DATA_BATCH, BATCH_ACK)
- **Raspberry Pi integration** with serial protocol
- **DuckDB sensor database** with query capabilities
- **REST API** for sensor data access and export
- **Web dashboard** with sensor charts and node management

### 🚧 Next Phase (Immediate)
- **Controller mode implementation** (scheduling, coordination)
- Real sensor integration (DS18B20, SHT30)
- Error handling improvements
- Input validation and security
- SPI communication robustness
- Interrupt-driven operations

### 🔮 Later Phase (Future)
- Soil moisture sensors integration
- Advanced irrigation scheduling algorithms
- Power management and sleep modes
- Current monitoring for valve diagnostics
- Encryption and authentication
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