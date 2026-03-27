# Increment 5: Interactive Calibration

## Goal
Allow users to calibrate the curtain travel time through an interactive procedure in the dashboard.

## Calibration Procedure
1. User clicks "Calibrate" in dashboard
2. Node drives curtain fully closed (REVERSE for max_motor_run_ms)
3. Node pauses briefly, then drives curtain open (FORWARD)
4. Timer starts counting
5. User watches curtain physically, clicks "Stop" when fully open
6. Node records elapsed time as `travel_time_ms_`, persists to flash config
7. Node sends calibration-complete event to hub with recorded travel time

## Firmware Changes

### `src/hal/curtain_controller.h` / `.cpp`
- Implement `startCalibration()` state machine:
  - CALIBRATING_CLOSE → motor runs in close direction for max_motor_run_ms
  - CALIBRATING_PAUSE → brief pause (1s)
  - CALIBRATING_OPEN → motor runs in open direction, timer counting
  - On `stop()` during CALIBRATING_OPEN: record elapsed as travel_time_ms
- Add `persistTravelTime()` — write travel_time_ms to a flash config sector
- Add `loadTravelTime()` — read from flash on startup

### `src/modes/greenhouse_mode.cpp`
- Handle `CMD_CALIBRATE` in `onActuatorCommand()` → call `curtain_controller_.startCalibration()`
- `onLoop()`: `update()` already handles calibration state transitions
- Send calibration-complete event with recorded travel time

## API Changes

### `api/app.py`
- Add `PUT /api/nodes/<address>/curtain/config` endpoint:
  - Body: `{"travel_time_seconds": N}`
  - Alternative to interactive calibration: user enters time manually
  - Sends config command to node via hub

## Dashboard Changes

### `dashboard/src/components/CurtainControl.tsx`
- Add "Calibrate" button that opens a calibration wizard:
  1. "Starting calibration... curtain closing" (progress indicator)
  2. "Curtain opening — click Stop when fully open" (prominent Stop button)
  3. "Calibration complete! Travel time: Xs" (confirmation)
- Add manual travel time input as alternative
- Show current calibrated travel time

## Verification
1. Click Calibrate → curtain closes fully, then opens
2. Click Stop when fully open → travel time recorded
3. Subsequent open/close operations use calibrated time and stop correctly
4. Travel time persists across reboots
5. Manual time entry via dashboard also works
