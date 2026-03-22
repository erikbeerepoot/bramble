# FRAM Support — FM24CL16B-GTR

## Overview

Add persistent non-volatile storage to the PMU using an FM24CL16B-GTR FRAM chip connected via I2C. This replaces the current RAM-only storage for schedules, wake intervals, and node state — all of which are currently lost on power-down.

## Hardware

- **Chip**: FM24CL16B-GTR — 16Kbit (2048 bytes) I2C FRAM
- **Interface**: I2C1
- **Pins**: PA9 (SCL, pin 17), PA10 (SDA, pin 18)
- **Address**: 0x50 base. The FM24CL16B uses the 3 LSBs of the I2C device address as page select (8 pages × 256 bytes), so it occupies addresses 0x50–0x57.
- **Speed**: Up to 1 MHz I2C. We'll use 100 kHz (standard mode) to keep it simple and reliable.
- **No write delays**: Unlike EEPROM, FRAM writes are instantaneous — no polling or wait needed after writes.

## Memory Map (2048 bytes total)

| Offset  | Size    | Content                      |
|---------|---------|------------------------------|
| 0x0000  | 4       | Magic number (0xFRAM0001)    |
| 0x0004  | 4       | Format version               |
| 0x0008  | 4       | Wake interval (seconds)      |
| 0x000C  | 2       | Number of schedule entries    |
| 0x000E  | 2       | Reserved                     |
| 0x0010  | 14      | Schedule entry 0 (7 bytes × 2 for alignment) |
| 0x001E  | 14      | Schedule entry 1             |
| 0x002C  | 32      | Node state blob              |
| 0x004C  | 4       | Node state valid flag        |
| 0x0050  | ~1968   | Free for future use          |

The magic number and format version let us detect uninitialized or incompatible FRAM on first boot.

## Implementation Plan

### Phase 1: I2C HAL Setup

**Files to modify:**
- `Core/Inc/stm32l0xx_hal_conf.h` — uncomment `HAL_I2C_MODULE_ENABLED`
- `Core/Inc/main.h` — add I2C pin definitions
- `Core/Src/stm32l0xx_hal_msp.c` — add `HAL_I2C_MspInit()` / `HAL_I2C_MspDeInit()` for GPIO and clock setup
- `Core/Src/main.cpp` — add I2C handle declaration and `MX_I2C1_Init()` function

**Details:**
1. Enable I2C HAL module in `stm32l0xx_hal_conf.h`
2. Define `FRAM_SCL_PIN` (GPIO_PIN_9) and `FRAM_SDA_PIN` (GPIO_PIN_10) in `main.h`
3. Configure PA9/PA10 as I2C1_SCL/SDA (AF1 on STM32L010) with open-drain, pull-up, in `HAL_I2C_MspInit()`
4. Initialize I2C1 at 100 kHz in `MX_I2C1_Init()`, call it from `main()`

### Phase 2: FRAM Driver

**New files:**
- `Core/Inc/fram.h`
- `Core/Src/fram.cpp`

**Class: `FRAM`**

```cpp
class FRAM {
public:
    FRAM(I2C_HandleTypeDef& i2c);

    bool init();           // Probe device, return true if present
    bool read(uint16_t address, uint8_t* data, uint16_t length);
    bool write(uint16_t address, const uint8_t* data, uint16_t length);

private:
    I2C_HandleTypeDef& i2c_;
    static constexpr uint8_t BASE_ADDRESS = 0x50;
    static constexpr uint16_t CAPACITY = 2048;

    // FM24CL16B addressing: device address bits [3:1] = page, then 8-bit word address
    uint8_t deviceAddress(uint16_t memAddress) const;
    uint8_t wordAddress(uint16_t memAddress) const;
};
```

The FM24CL16B uses a slightly non-standard I2C addressing scheme: the 11-bit memory address is split between the device address (upper 3 bits as page select in bits [3:1]) and the word address byte (lower 8 bits). The driver must handle this by computing the correct I2C slave address per page and sending the appropriate word address byte.

**Key considerations:**
- Reads/writes that cross a 256-byte page boundary must be split into two I2C transactions
- Use `HAL_I2C_Mem_Write` / `HAL_I2C_Mem_Read` with 1-byte memory address size, adjusting the device address per page
- No write delay needed (FRAM advantage over EEPROM)

### Phase 3: Persistent Storage Abstraction

**New files:**
- `Core/Inc/persistent_storage.h`
- `Core/Src/persistent_storage.cpp`

**Class: `PersistentStorage`**

```cpp
class PersistentStorage {
public:
    PersistentStorage(FRAM& fram);

    bool init();  // Check magic, format if needed

    // Wake interval
    bool loadWakeInterval(uint32_t& seconds);
    bool saveWakeInterval(uint32_t seconds);

    // Schedules
    bool loadSchedules(WateringSchedule::Entry* entries, uint8_t& count);
    bool saveSchedules(const WateringSchedule::Entry* entries, uint8_t count);

    // Node state
    bool loadNodeState(uint8_t* state, uint8_t length);
    bool saveNodeState(const uint8_t* state, uint8_t length);
    bool isNodeStateValid();
    void invalidateNodeState();

private:
    FRAM& fram_;
    bool formatStorage();  // Write magic + version, zero data
};
```

This layer owns the memory map layout and provides typed accessors. The protocol layer calls this instead of managing its own RAM copies.

### Phase 4: Integration with Protocol & Main

**Files to modify:**
- `Core/Src/main.cpp` — instantiate FRAM, PersistentStorage, pass to Protocol
- `Core/Inc/pmu_protocol.h` / `Core/Src/pmu_protocol.cpp` — use PersistentStorage for schedule/state/wake-interval persistence

**Integration points in protocol:**
1. **`handleSetWakeInterval()`** — after updating RAM copy, call `storage.saveWakeInterval()`
2. **`handleSetSchedule()`** — after updating RAM copy, call `storage.saveSchedules()`
3. **`handleSaveNodeState()`** — call `storage.saveNodeState()` instead of only keeping in RAM
4. **`handleLoadNodeState()`** — call `storage.loadNodeState()` to retrieve from FRAM
5. **Startup** — load wake interval and schedules from FRAM into RAM working copies

**Startup flow change:**
```
main() init:
  1. Init I2C1
  2. Init FRAM (probe)
  3. Init PersistentStorage (check magic, format if needed)
  4. Load wake interval from FRAM → pass to RTC config
  5. Load schedules from FRAM → pass to protocol handler
  6. Continue with existing init (LED, DCDC, UART, state machine)
```

### Phase 5: Cold-Start Detection Update

Currently cold-start is detected via RTC backup register magic (0xBEEF2025). With FRAM:
- Keep the RTC magic for RTC time validity (separate concern)
- Use FRAM node state valid flag for node state cold-start detection
- On power-on-reset: check FRAM for valid node state instead of only checking RAM flag

## Build Changes

- Add `Core/Src/fram.cpp` and `Core/Src/persistent_storage.cpp` to `CMakeLists.txt` source list
- The STM32 HAL I2C driver sources should already be pulled in by the CubeMX cmake config once the module is enabled

## Resource Impact

- **Flash**: ~1–2 KB additional code (I2C HAL + FRAM driver + storage layer)
- **RAM**: Minimal — I2C handle struct (~80 bytes), FRAM and PersistentStorage objects are small
- **Power**: I2C bus idle current is negligible; FRAM standby is ~7 µA. Ensure I2C pins are configured properly before entering STOP mode (either de-init or ensure no floating lines).

## Risks & Open Questions

1. **STOP mode and I2C pins**: Need to verify PA9/PA10 don't cause excess current draw in STOP mode. May need to de-init I2C before sleep and re-init on wake.
2. **FRAM not populated**: The driver's `init()` probes the device. If absent, PersistentStorage falls back to RAM-only behavior (current behavior). This keeps firmware compatible with boards without FRAM.
3. **STM32L010F4 I2C1 pin mapping**: Confirm PA9=SCL and PA10=SDA are on AF1 for I2C1 on this specific part. (They should be per the reference manual.)

## Phasing

Phases 1–3 can be implemented and tested independently (probe FRAM, read/write raw bytes, verify storage abstraction). Phase 4 integrates with the existing protocol and is where the real value lands. Phase 5 is a small cleanup.

I'd suggest implementing phases 1–3 first and verifying on hardware before integrating with the protocol layer.
