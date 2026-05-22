# Factory Reset Implementation

## Context
The PMU (STM32L010) has FRAM persistent storage (FM24CL16B, 2KB) storing wake interval, schedules, node state blob, and a validity flag. `SystemReset` (command 0x1A) resets the MCU but preserves FRAM. There is no way to clear persisted state — this is needed to fix issues like stale LoRa addresses.

## What exists
- `PersistentStorage::formatStorage()` — private method that zeros FRAM and writes fresh magic/version. Called automatically on first boot.
- `invalidateNodeState()` — clears the valid flag but not the state blob
- `WateringSchedule::clear()` — sets schedule count to 0
- FRAM driver, I2C1, and PersistentStorage are fully wired up in main.cpp

## What's needed
1. ~~**New PMU protocol command** `FactoryReset` (e.g., 0x1B) in `pmu_protocol.h`~~ ✅
2. ~~**Handler** in `pmu_protocol.cpp` that calls `formatStorage()` (make it public or add a `factoryReset()` method on PersistentStorage)~~ ✅
3. ~~**ACK, then reset** — send ACK before formatting, then `NVIC_SystemReset()` after~~ ✅
4. ~~**RP2040 side** — add `factoryReset()` to `ReliablePmuClient` and expose in `SensorPmuManager`~~ ✅
5. **Trigger mechanism** — e.g., hub command via LoRa, or detect a GPIO button hold at boot (TODO)

## Key files
- `pmu-stm32/Core/Inc/pmu_protocol.h` — command enum
- `pmu-stm32/Core/Src/pmu_protocol.cpp` — handler
- `pmu-stm32/Core/Inc/persistent_storage.h` — make formatStorage public
- `src/hal/pmu_protocol.h` — RP2040 command enum
- `src/hal/pmu_reliability.h/.cpp` — reliable client method
