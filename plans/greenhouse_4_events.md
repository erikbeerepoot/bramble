# Increment 4: Event Recording

## Goal
Record curtain open/close/stop events so there's operational visibility into what the greenhouse controller has done.

## Firmware Changes

### `src/lora/message.h`
- Add `SENSOR_EVENT = 0x10` to `SensorType` enum

### `src/modes/greenhouse_mode.cpp`
- After each curtain action (open/close/stop), send a `MSG_TYPE_SENSOR_DATA` message to hub with:
  - `sensor_type = SENSOR_EVENT`
  - Payload: `{event_type (1 byte), timestamp (4 bytes), elapsed_ms (4 bytes)}`
- Use BEST_EFFORT delivery (events are informational, not critical)

## API Changes

### `api/database.py`
- Add `actuator_events` table:
  - `device_id BIGINT, timestamp INTEGER, event_type VARCHAR, details VARCHAR, received_at INTEGER`
- Add `insert_actuator_event()` and `get_actuator_events(device_id, start, end)` methods

### `api/serial_interface.py`
- Parse incoming SENSOR_EVENT data from hub serial output
- Insert into `actuator_events` table

### `api/app.py`
- Add `GET /api/nodes/<device_id>/events` endpoint
  - Query params: `start`, `end` (time range)
  - Returns list of events with timestamp, type, details

## Dashboard Changes

### `dashboard/src/components/CurtainControl.tsx`
- Add event history section below the control buttons
- Fetch and display recent events (last 20) with timestamps

### `dashboard/src/api/client.ts`
- Add `getNodeEvents(deviceId, start?, end?)` function

## Verification
1. Open/close/stop curtain from dashboard
2. Events appear in `GET /api/nodes/<device_id>/events` response
3. Event history visible in dashboard CurtainControl component
4. Events persist across node/API restarts
