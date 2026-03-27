# Increment 1: Greenhouse Firmware Core

## Goal
Build and flash a GREENHOUSE variant that initializes the curtain motor controller, registers with the hub, and handles actuator commands over LoRa.

## New Protocol Constants
**File**: `src/lora/message.h`
- Add `ACTUATOR_CURTAIN = 0x04` to `ActuatorType`
- Add `CMD_STOP = 0x04`, `CMD_CALIBRATE = 0x05` to `ActuatorCommand`

## New Files

### `src/hal/curtain_controller.h` / `.cpp`
2-GPIO non-blocking motor driver.

**States**: `UNKNOWN, OPEN, CLOSED, OPENING, CLOSING, STOPPED, CALIBRATING, ERROR`

**Interface**:
- `initialize(open_pin, close_pin)` — configure GPIOs as output, both LOW
- `open()` — assert close LOW, set open HIGH, record start time, state→OPENING
- `close()` — assert open LOW, set close HIGH, record start time, state→CLOSING
- `stop()` — both LOW, update estimated_position from elapsed/travel_time, state→STOPPED
- `update()` — called every loop; if motor running: check elapsed vs travel_time (stop if done), check safety timeout (stop + ERROR if exceeded)
- `getState()`, `getEstimatedPosition()`, `getTravelTime()`, `setTravelTime()`

**Safety**: `max_motor_run_ms_` = 180000 (3 min). Motor always stops if exceeded.

**Pin assignments**: Define `PIN_CURTAIN_OPEN` and `PIN_CURTAIN_CLOSE` in `src/board/bramble_v4_pins.h`. Reuse two existing motor GPIOs (PIN_MOTOR_HI_1, PIN_MOTOR_HI_2) or pick available GPIOs — exact pins TBD with user based on wiring.

### `src/modes/greenhouse_mode.h` / `.cpp`
Extends `ApplicationMode`. Pattern follows `irrigation_mode`.

- `shouldSleep()` → `false`
- `onStart()` → init curtain controller, init PMU for RTC, set LED pattern (green pulse), send heartbeat
- `onLoop()` → `curtain_controller_.update()`
- `onActuatorCommand()` → dispatch ACTUATOR_CURTAIN commands to curtain controller
- `onHeartbeatResponse()` → sync RTC via PMU (reuse irrigation pattern)

### `src/util/greenhouse_state_machine.h` / `.cpp`
States: INITIALIZING → AWAITING_TIME → IDLE ↔ CURTAIN_MOVING / ERROR
Follow `IrrigationStateMachine` pattern.

## Modified Files

### `CMakeLists.txt`
- Add GREENHOUSE to `set_property(CACHE HARDWARE_VARIANT PROPERTY STRINGS ...)`
- Add build block for `bramble_greenhouse` executable with sources: `greenhouse_mode.cpp`, `curtain_controller.cpp`, `greenhouse_state_machine.cpp`, `${COMMON_SOURCES}`
- Define `HARDWARE_GREENHOUSE=1`, `DEFAULT_IS_HUB=0`

### `main.cpp`
- Add `#ifdef HARDWARE_GREENHOUSE` include for `modes/greenhouse_mode.h`
- Add to `getVariantInfo()`: `{NODE_TYPE_ACTUATOR, CAP_VALVE_CONTROL, "Greenhouse Node"}`
- Add variant log line
- Add mode instantiation block (no PMU sleep signaling)

## Verification
1. `cmake -B build -DHARDWARE_VARIANT=GREENHOUSE` → builds successfully
2. `cmake -B build -DHARDWARE_VARIANT=ALL` → still builds all variants
3. Flash to V4 board → serial shows "GREENHOUSE mode", registers with hub
4. Heartbeat shows correct node type and capabilities
