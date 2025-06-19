# Runtime Configuration System - Future Implementation Plan

## Overview
This document outlines a future plan for implementing runtime configuration in the Bramble LoRa farm monitoring system. **This is not currently implemented** - it's a design for future enhancement when remote configuration capabilities become needed.

## Why Runtime Config?
The main value proposition is **remote configuration of deployed field nodes** without physical access. However, this requires infrastructure we don't currently have (WiFi hub connectivity, etc.).

## Current Status: DEFERRED
- **Decision**: Skip runtime config for now, focus on immediate priorities
- **Reason**: Requires infrastructure (WiFi hub) we don't have yet
- **Alternative**: Use compile-time configuration variants for different deployments

---

## Minimal Viable Configuration System (Future)

### Core Principle
Only make configurable what you **can't easily change by reflashing**. The real value is remote configuration via LoRa for deployed nodes in the field.

### High-Value Configurable Parameters

#### 1. LoRa Radio Settings (Regulatory/Performance)
- `frequency` - Different regions (915MHz US vs 868MHz EU)
- `tx_power` - Adjust for range vs power consumption  
- `spreading_factor` - Trade speed vs range

#### 2. Operational Timing (Field Tuning)
- `sensor_interval_ms` - Battery life vs data freshness
- `heartbeat_interval_ms` - Network overhead vs monitoring
- `node_offline_timeout_ms` - How long before marking node dead

#### 3. Network Reliability (Network Conditions)
- `max_retries` - Adjust for network reliability
- `ack_timeout_ms` - Adjust for network latency

### Configuration Structure

```cpp
// src/config/runtime_config.h
struct RuntimeConfig {
    // LoRa Radio (most important for deployed systems)
    uint32_t lora_frequency;        // 915000000 or 868000000
    uint8_t lora_tx_power;          // 2-20 dBm
    uint8_t lora_spreading_factor;  // 7-12
    
    // Timing (second most important)
    uint32_t sensor_interval_ms;    // How often to read sensors
    uint32_t heartbeat_interval_ms; // How often to send heartbeat
    uint32_t node_offline_timeout_ms; // When to mark nodes offline
    
    // Network reliability
    uint8_t max_retries;            // Message retry count
    uint32_t ack_timeout_ms;        // ACK wait time
    
    // Validation
    uint32_t magic;                 // 0xCFG1234
    uint32_t crc32;                 // Data integrity
};
```

### Simple Config Manager

```cpp
class RuntimeConfigManager {
public:
    static RuntimeConfigManager& getInstance();
    
    bool initialize(Flash& flash);
    const RuntimeConfig& get() const { return config_; }
    
    // Remote update via LoRa messages
    bool updateConfig(const RuntimeConfig& new_config);
    bool updateLoRaSettings(uint32_t freq, uint8_t power, uint8_t sf);
    bool updateTimings(uint32_t sensor_ms, uint32_t heartbeat_ms);
    
private:
    RuntimeConfig config_;
    Flash* flash_;
    void setDefaults();
    bool validate(const RuntimeConfig& config) const;
};
```

### LoRa-based Remote Configuration

Add new message type:
```cpp
MSG_TYPE_CONFIG_UPDATE = 0x09

struct ConfigUpdatePayload {
    uint8_t update_type;        // 1=LoRa, 2=Timing, 3=Network
    union {
        struct {
            uint32_t frequency;
            uint8_t tx_power;
            uint8_t spreading_factor;
        } lora;
        struct {
            uint32_t sensor_interval_ms;
            uint32_t heartbeat_interval_ms;
            uint32_t node_offline_timeout_ms;
        } timing;
        struct {
            uint8_t max_retries;
            uint32_t ack_timeout_ms;
        } network;
    };
    uint32_t auth_token;        // Simple authentication
};
```

## Remote Update Workflow

1. **Hub sends config update** to specific node or broadcast
2. **Node validates** the new configuration
3. **Node applies** safe changes immediately (intervals, timeouts)
4. **Node saves** to flash and **ACKs** success
5. **Node reboots** for radio changes (frequency/power) - **only if needed**

## Implementation Priority (When Resumed)

### Phase 1: Core Config (1-2 days)
- Create `RuntimeConfig` structure with defaults
- Add flash save/load with CRC validation  
- Replace current hardcoded timing values
- Test basic functionality

### Phase 2: LoRa Radio Config (1 day)
- Add radio parameter updates
- Handle frequency/power changes (may require restart)
- Test different radio settings

### Phase 3: Remote Updates (2-3 days)
- Add `MSG_TYPE_CONFIG_UPDATE` message handling
- Implement config update protocol with authentication
- Add hub-side config management commands
- Test remote configuration over LoRa

## Key Benefits (When Implemented)

1. **Field Deployment**: Change sensor intervals based on battery life observations
2. **Regional Compliance**: Remotely switch frequency for different countries
3. **Network Optimization**: Tune retry/timeout based on observed network performance
4. **Power Management**: Adjust intervals to extend battery life
5. **Range Optimization**: Tune spreading factor and power for coverage

## What Should Stay Hardcoded

- **GPIO pins** - Hardware-specific, needs reflash anyway
- **SPI frequency** - Hardware-specific, rarely changed
- **Protocol constants** - Changing breaks compatibility
- **Flash layout** - Requires careful planning, reflash safer
- **Address ranges** - Network architecture decision

## Prerequisites for Implementation

1. **Hub connectivity** - WiFi module, Ethernet, or cellular
2. **Hub API** - Web interface or API to send config commands
3. **Authentication** - Secure way to authorize config changes
4. **Field testing** - Validate that remote config is actually needed

## Alternative Approaches (Immediate)

### Option A: Enhanced Build Configs
```cpp
// config_profiles.h
#ifdef CONFIG_PROFILE_US_FARM
    #define LORA_FREQUENCY 915000000
    #define SENSOR_INTERVAL_MS 300000      // 5 min (battery conservative)
    #define TX_POWER 14                    // Lower power
#endif

#ifdef CONFIG_PROFILE_EU_GREENHOUSE  
    #define LORA_FREQUENCY 868000000
    #define SENSOR_INTERVAL_MS 60000       // 1 min (powered nodes)
    #define TX_POWER 20                    // Max range
#endif
```

### Option B: USB Serial Config Interface
```cpp
// When hub connected to computer via USB
config set sensor_interval 300000
config set frequency 868000000
config save
config reboot
```

### Option C: Button-based Physical Config
```cpp
// Hold button during boot = cycle through predefined configs
// LED patterns indicate current config
// Useful for field switching between "summer" vs "winter" settings
```

## Usage Examples (Future)

```cpp
// In main loop
const RuntimeConfig& cfg = RuntimeConfigManager::getInstance().get();

// Use configurable timing
if (current_time - last_sensor_time >= cfg.sensor_interval_ms) {
    // Read sensors
}

// Use configurable radio settings  
lora.setFrequency(cfg.lora_frequency);
lora.setTxPower(cfg.lora_tx_power);

// Use configurable retry logic
messenger.setRetryParams(cfg.max_retries, cfg.ack_timeout_ms);
```

## Files to Create (When Implemented)

**New Files:**
- `src/config/runtime_config.h` - Configuration data structures
- `src/config/config_manager.h/.cpp` - Configuration management class
- `src/config/config_defaults.h` - Default configuration values

**Modified Files:**
- `bramble.cpp` - Replace hardcoded values with config lookups
- `src/lora/sx1276.cpp` - Use config for radio parameters
- `src/lora/reliable_messenger.cpp` - Use config for retry/timeout settings
- `src/lora/message.h` - Add MSG_TYPE_CONFIG_UPDATE
- `CMakeLists.txt` - Add new config source files

---

## Decision Log

**2024-06-19**: Decided to defer runtime configuration implementation
- **Reason**: Requires hub connectivity infrastructure we don't have
- **Alternative**: Focus on immediate TODOs and revisit when hub gets WiFi/network
- **Next Action**: Move to next item in todo list

This plan provides a comprehensive approach for **future implementation** when the supporting infrastructure is available.