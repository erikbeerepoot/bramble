# Increment 2: Hub Command Path

## Goal
Add a `SEND_ACTUATOR` serial command to the hub so it can forward direct actuator commands over LoRa to the greenhouse node. After this increment, the greenhouse curtain can be controlled from the hub's serial console.

## Modified Files

### `src/modes/hub_mode.h`
- Add `handleSendActuator(const char *args)` private method declaration

### `src/modes/hub_mode.cpp`
- Add `SEND_ACTUATOR` to `handleSerialCommand()` dispatcher
- Implement `handleSendActuator()`:
  - Parse: `SEND_ACTUATOR <node_addr> <actuator_type> <command> [param]`
  - Send `MSG_TYPE_ACTUATOR_CMD` via `messenger_.sendActuatorCommand()` with RELIABLE delivery
  - Respond with `SENT ACTUATOR ...` or `ERROR ...`

- Update `handleListNodes()` to detect greenhouse nodes:
  - Check `node_type == NODE_TYPE_ACTUATOR` to report as "GREENHOUSE" type
  - (Irrigation nodes are `NODE_TYPE_HYBRID`, so no conflict)

## Verification
1. Build hub variant: `cmake -B build -DHARDWARE_VARIANT=HUB && cmake --build build -j8`
2. Build ALL variant still works
3. With greenhouse node running, send via hub serial:
   - `SEND_ACTUATOR <addr> 4 1` → curtain opens
   - `SEND_ACTUATOR <addr> 4 4` → curtain stops
   - `SEND_ACTUATOR <addr> 4 0` → curtain closes
4. `LIST_NODES` shows greenhouse node as type "GREENHOUSE"
