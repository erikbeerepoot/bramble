# Firmware Version Reporting

## Overview
Add firmware version reporting to node registration and expose it through the entire stack to the dashboard.

## Version Format
- `uint32_t` with encoding: `(MAJOR << 24) | (MINOR << 16) | BUILD`
- Major: 8 bits (0-255), Minor: 8 bits (0-255), Build: 16 bits (0-65535)
- Display format: "v1.0.0", "v1.1.5", etc.
- Single source of truth: `src/version.h`

## Changes

### Firmware (C++)

1. **New file: `src/version.h`**
   - Define `BRAMBLE_VERSION_MAJOR`, `BRAMBLE_VERSION_MINOR`, `BRAMBLE_VERSION_BUILD`
   - Define `BRAMBLE_FIRMWARE_VERSION` as packed uint32_t
   - Helper macro to format version string

2. **`src/lora/message.h`** - `RegistrationPayload.firmware_ver`: `uint16_t` -> `uint32_t`

3. **`src/lora/message.cpp`** - Update `createRegistrationMessage` signature

4. **`src/lora/address_manager.h`** - `NodeInfo.firmware_version`: `uint16_t` -> `uint32_t`, update `registerNode` signature

5. **`src/lora/address_manager.cpp`** - Update `registerNode` signature

6. **`src/lora/reliable_messenger.h`** - Update `sendRegistrationRequest` signature

7. **`src/lora/reliable_messenger.cpp`** - Update `sendRegistrationRequest` signature

8. **`src/config/node_config.h`** - `NodeConfiguration.firmware_version`: `uint16_t` -> `uint32_t`, update `createDefaultConfiguration` signature

9. **`src/config/node_config.cpp`** - Update `createDefaultConfiguration` signature

10. **`src/config/hub_config.h`** - `RegistryNodeEntry.firmware_version`: `uint16_t` -> `uint32_t`

11. **`main.cpp`** - Replace hardcoded `0x0100` with `BRAMBLE_FIRMWARE_VERSION` (2 places)

12. **`src/modes/irrigation_mode.cpp`** - Replace hardcoded `0x0100` with `BRAMBLE_FIRMWARE_VERSION` (1 place)

13. **`src/modes/hub_mode.cpp`** - Add firmware_version to `handleListNodes()` UART output

### Python API

14. **`api/models.py`** - Add `firmware_version` field to `Node`

15. **`api/app.py`** - Parse firmware_version from LIST_NODES response

### Dashboard (TypeScript/React)

16. **`dashboard/src/types/index.ts`** - Add `firmware_version` to `Node` interface, add `formatFirmwareVersion()` helper

17. **`dashboard/src/components/NodeDetail.tsx`** - Display firmware version in Node Status panel

18. **`dashboard/src/components/NodeCard.tsx`** - Display firmware version alongside device ID

## Flash Compatibility Note
Changing `RegistryNodeEntry.firmware_version` from `uint16_t` to `uint32_t` changes the struct size. Existing persisted registries will fail CRC/magic checks and start fresh. This is acceptable.

Similarly, `NodeConfiguration.firmware_version` change will invalidate saved node configs. Nodes will re-register on next boot.
