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

## Next Steps 🎯
1. **Implement ACK/Retry for actuators** - Add reliable delivery
2. **Create Hub vs Node firmware variants** - Different behaviors
3. **Add real sensor integration** - Temperature/humidity sensor
4. **Implement actuator control** - Valve switching capability
5. **Add power management** - Sleep modes for battery operation

## Success Criteria
- ✅ ~~Basic LoRa communication working~~
- [ ] Hub can communicate with 10+ nodes simultaneously  
- ✅ ~~Sensor data transmitted reliably~~ (short range proven)
- [ ] Actuator commands have 99%+ delivery success
- [ ] Battery nodes achieve 6+ month operation
- [ ] Network auto-recovers from hub restart