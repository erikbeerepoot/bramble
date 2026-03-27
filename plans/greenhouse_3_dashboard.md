# Increment 3: Dashboard Controls

## Goal
Add API endpoint and React UI for open/close/stop curtain control. After this increment, users can control the greenhouse curtain from their browser.

## API Changes

### `api/command_queue.py`
- Add `queue_send_actuator(node_address, actuator_type, command, param=0)`:
  - Sends `SEND_ACTUATOR <addr> <type> <cmd> <param>` via hub serial

### `api/app.py`
- Add `POST /api/nodes/<address>/curtain` endpoint:
  - Body: `{"action": "open" | "close" | "stop"}`
  - Maps actions to actuator commands (ACTUATOR_CURTAIN=4, CMD_TURN_ON=1, CMD_TURN_OFF=0, CMD_STOP=4)
  - Calls `queue_send_actuator()`, returns 202 with task_id

## Dashboard Changes

### `dashboard/src/api/client.ts`
- Add `controlCurtain(address, action)` function → POST to `/api/nodes/<address>/curtain`

### `dashboard/src/components/CurtainControl.tsx` (new)
- Three buttons: Open (green), Stop (amber), Close (red)
- Status display: current curtain state, estimated position
- Rendered as a card within NodeDetail for greenhouse-type nodes

### `dashboard/src/components/NodeDetail.tsx`
- Import and render `<CurtainControl>` when node type is "GREENHOUSE"

### `dashboard/src/types/index.ts` (if node types are defined here)
- Add GREENHOUSE to node type constants

## Verification
1. Start API server and dashboard dev server
2. Greenhouse node visible in node list with correct type
3. NodeDetail page shows curtain control card with Open/Stop/Close buttons
4. Clicking buttons sends correct API calls, hub forwards LoRa commands
5. Curtain motor responds to each command
