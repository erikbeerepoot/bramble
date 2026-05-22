---
name: bramble-flash
description: Flash RP2040 firmware using OpenOCD and CMSIS-DAP debug probe.
---

# Bramble Flash

Flash bramble firmware to the connected RP2040 device using OpenOCD.

## Arguments

- `$ARGUMENTS` - Hardware variant: SENSOR, IRRIGATION, CONTROLLER, HUB, or GENERIC (default: SENSOR)

## Instructions

1. **Parse the variant argument**
   - If `$ARGUMENTS` is provided and matches a valid variant (SENSOR, IRRIGATION, CONTROLLER, HUB, GENERIC), use it
   - Otherwise default to SENSOR
   - The ELF file will be `build/bramble_<variant>.elf` (lowercase)

2. **Verify the ELF file exists**
   Check that `build/bramble_<variant>.elf` exists. If not, tell the user to run `/bramble-build <VARIANT>` first.

3. **Flash the firmware**
   Use OpenOCD to flash via CMSIS-DAP:
   ```bash
   ~/.pico-sdk/openocd/0.12.0+dev/openocd \
     -s ~/.pico-sdk/openocd/0.12.0+dev/scripts \
     -f interface/cmsis-dap.cfg \
     -f target/rp2040.cfg \
     -c "adapter speed 5000" \
     -c "program build/bramble_<variant>.elf verify reset exit" 2>&1
   ```

4. **Report results**
   - On success: Report that firmware was flashed and verified successfully
   - On failure: Report the OpenOCD error output

## Error Handling

- If ELF file missing: Tell user to build first with `/bramble-build <VARIANT>`
- If "no device found": Debug probe is not connected or not recognized
- Other errors: Report the OpenOCD error output

## Example Usage

```
/bramble-flash           # Flash SENSOR variant
/bramble-flash SENSOR    # Flash SENSOR variant
/bramble-flash IRRIGATION # Flash IRRIGATION variant
```
