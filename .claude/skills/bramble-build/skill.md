---
name: bramble-build
description: Build RP2040 firmware using CMake. Supports all hardware variants.
---

# Bramble Build

Build the bramble firmware for RP2040.

## Arguments

- `$ARGUMENTS` - Hardware variant: SENSOR, IRRIGATION, CONTROLLER, HUB, GENERIC, or ALL (default: ALL)

## Instructions

1. **Parse the variant argument**
   - If `$ARGUMENTS` is "ALL" or empty/not provided, build all variants using `HARDWARE_VARIANT=ALL`
   - If `$ARGUMENTS` matches a valid single variant (SENSOR, IRRIGATION, CONTROLLER, HUB, GENERIC), build only that variant
   - The executable names are `bramble_<variant>` (lowercase), e.g., `bramble_sensor`

2. **Build the firmware**

   All builds use the same `build/` directory. CMake supports `HARDWARE_VARIANT=ALL` which defines all variant targets in a single configure, so `-j8` parallelizes across all variants automatically.

   ```bash
   ~/.pico-sdk/cmake/v3.31.5/bin/cmake -B build -DHARDWARE_VARIANT=<VARIANT> 2>&1
   ~/.pico-sdk/cmake/v3.31.5/bin/cmake --build build -j8 2>&1
   ```

   Where `<VARIANT>` is one of: ALL, SENSOR, IRRIGATION, CONTROLLER, HUB, GENERIC.

3. **Report results**
   - On success: Report which variants built successfully and their output file locations
   - On failure: Report which variants failed and show compiler errors
   - For ALL builds, summarize: "X/5 variants built successfully"

## Example Usage

```
/bramble-build           # Build ALL variants
/bramble-build ALL       # Build ALL variants
/bramble-build SENSOR    # Build only SENSOR variant
/bramble-build IRRIGATION # Build only IRRIGATION variant
```
