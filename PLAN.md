# LoRa Communication Layer - Implementation Plan

## Architecture Overview

### Star Topology Design
- **Hub**: Central coordinator managing all node communications
- **Nodes**: Sensor/actuator endpoints (dozens supported)
- **Optional**: Hub can route messages between nodes (Phase 2)

### Hardware Specifications  
- **LoRa Chip**: SX1276 on RP2040 Feather boards
- **SPI Interface**: Already configured (SPI0, 1MHz, pins 16-19)
- **Power**: Mixed battery/mains powered nodes

## Message Protocol Design

### Message Types
1. **Sensor Data** (fire-and-forget, loss acceptable)
   - Temperature/humidity readings
   - Battery status
   - Periodic heartbeat

2. **Actuator Commands** (reliable delivery required)
   - Valve open/close commands  
   - Configuration updates
   - Acknowledgment required with retries

3. **Network Management**
   - Node registration/discovery
   - Time synchronization
   - Network topology updates

### Message Format
```
[Header: 8 bytes][Payload: 0-247 bytes]

Header:
- Magic: 2 bytes (0xBEEF)
- Message Type: 1 byte
- Source Address: 2 bytes  
- Destination Address: 2 bytes
- Sequence Number: 1 byte

Payload: Variable length based on message type
```

### Addressing Scheme
- Hub Address: 0x0000 (reserved)
- Node Addresses: 0x0001-0xFFFE (65534 max nodes)
- Broadcast Address: 0xFFFF

### Reliability Mechanism
- **Sensors**: Send without ACK, optional duplicate detection
- **Actuators**: Send with ACK required, exponential backoff retry (3 attempts max)
- **Timeout**: 5 seconds for ACK, then retry

## Implementation Phases

### Phase 1: Core Infrastructure
1. **SX1276 Driver** (`sx1276.h/.cpp`)
   - Basic register interface
   - TX/RX packet handling
   - Interrupt-driven operation

2. **Message Layer** (`message.h/.cpp`)
   - Packet encode/decode
   - Address management
   - Message type definitions

3. **Hub Core** (`hub.h/.cpp`)
   - Node registry and management
   - Message routing to specific nodes
   - Reliable delivery for actuators

4. **Node Core** (`node.h/.cpp`)
   - Auto-registration with hub
   - Power management (sleep/wake)
   - Sensor data transmission
   - Actuator command handling

### Phase 2: Advanced Features
1. **Message Routing** (hub forwards node-to-node)
2. **Network Discovery** (automatic topology detection)
3. **Time Synchronization** (for coordinated actions)
4. **Firmware Updates** (over-the-air capability)

## File Structure
```
bramble/
├── src/
│   ├── lora/
│   │   ├── sx1276.h/.cpp      # SX1276 driver
│   │   ├── message.h/.cpp     # Message protocol
│   │   ├── hub.h/.cpp         # Hub implementation  
│   │   └── node.h/.cpp        # Node implementation
│   ├── sensors/
│   │   └── temp_humidity.h/.cpp
│   └── actuators/
│       └── valve.h/.cpp
├── examples/
│   ├── hub_example.cpp
│   └── node_example.cpp
└── tests/
    └── message_tests.cpp
```

## Configuration Parameters
- **Frequency**: 915 MHz (US) / 868 MHz (EU) - configurable
- **Spreading Factor**: 7 (fastest) to 12 (longest range)
- **Bandwidth**: 125 kHz default
- **Coding Rate**: 4/5 default
- **TX Power**: 14 dBm default (configurable 2-20 dBm)

## Power Management Strategy
- **Nodes**: Deep sleep between transmissions
- **Wake Intervals**: Configurable (1 min to 1 hour for sensors)
- **Battery Monitoring**: Include level in all transmissions
- **Low Power Alerts**: Automatic notification to hub

## Testing Strategy
1. Unit tests for message encoding/decoding
2. Range testing with different power levels
3. Multi-node stress testing (packet collision handling)
4. Power consumption measurement
5. Reliability testing with intermittent connectivity

## Implementation Priority
1. ✅ Basic SX1276 driver and message protocol - **COMPLETED**
2. ✅ Simple hub-node communication (sensor data) - **COMPLETED** 
3. 🔄 Reliable actuator command delivery - **NEXT: ACK/retry mechanism**
4. ⏳ Power management for battery nodes
5. ⏳ Multi-node scaling and routing

## What's Completed ✅
- SX1276 driver with proper SPI1 configuration
- Message protocol with header/payload structure
- Bidirectional LoRa communication working
- NeoPixel HAL for status indication
- 20 dBm transmit power optimization
- Basic sensor message transmission
- **ReliableMessenger with ACK/retry mechanism** - Criticality-based delivery
- **Integration test framework** - MockSX1276 for controlled testing
- **Production/demo mode separation** - Clean architecture

## Current Priority: Auto Address Assignment 🎯
### Phase: Network Registration System
1. **Auto address assignment via registration**
2. **Persistent address storage in flash**
3. **Node-to-node routing via hub**
4. **Network discovery and management**

### Implementation Tasks:
1. **Registration Protocol Enhancement**
   - Expand registration message with unique device ID
   - Add registration response with assigned address
   - Handle registration failures and retries

2. **Address Management System**
   - Hub address allocation and tracking
   - Node address persistence (flash storage)
   - Address conflict detection and resolution

3. **Node-to-Node Routing**
   - Hub message forwarding capability
   - Routing table management
   - Route discovery mechanism

4. **Network Discovery**
   - Active node tracking in hub
   - Node capability registration
   - Network topology visualization

## Future Steps 🔮
1. **Add real sensor integration** - Temperature/humidity sensor
2. **Implement actuator control** - Valve switching capability
3. **Add power management** - Sleep modes for battery operation
4. **Over-air firmware updates** - Remote node maintenance

## Development Todo List

### NOW (Critical for Production)
- [ ] Add comprehensive input validation and buffer overflow protection
- [ ] Make hardcoded configuration values runtime configurable (intervals, addresses, etc)

### NEXT (Important Improvements)
- [ ] Standardize header guards to use #pragma once consistently
- [ ] Add error handling for SPI communications with timeout and retry logic
- [ ] Implement interrupt-driven LoRa operation instead of polling

### LATER (Future Features & Polish)
- [ ] Replace TODO placeholders: implement actual sensor readings in production mode
- [ ] Replace TODO placeholders: implement actuator command processing for valve/pump control
- [ ] Remove hardcoded flash size and implement runtime detection
- [ ] Migrate remaining printf statements to use Logger class throughout codebase

### COMPLETED ✅
- [x] Create test framework header with test runner
- [x] Implement reliability test suite
- [x] Add mock LoRa for controlled testing
- [x] Create production vs test build configuration
- [x] Clean up main.cpp for production use
- [x] Enhance registration protocol with device ID and address response
- [x] Implement address management system in hub
- [x] Add persistent address storage in flash memory
- [x] Create node-to-node routing via hub
- [x] Add network discovery and active node tracking
- [x] Implement heartbeat message protocol
- [x] Add node online/offline status tracking
- [x] Create basic network status reporting
- [x] Add proper error handling for flash operations with error recovery
- [x] Implement Logger class system for power-efficient debugging

## Success Criteria
- ✅ ~~Basic LoRa communication working~~
- ✅ ~~Hub can manage node registration and routing~~
- ✅ ~~Sensor data transmitted reliably~~ (short range proven)
- ✅ ~~Network discovery and heartbeat monitoring~~
- [ ] Actuator commands have 99%+ delivery success
- [ ] Battery nodes achieve 6+ month operation
- [ ] Network auto-recovers from hub restart