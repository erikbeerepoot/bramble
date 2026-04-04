# Irrigation Node Detail Page - Design Plan

## Context

The irrigation node boots, registers with the hub, and supports:
- **2 DC latching solenoid valves** (via H-bridge + valve indexer)
- **PMU-managed schedules** (up to 8 entries, stored in STM32 FRAM)
- **Update pull system**: hub queues updates (schedules, wake interval, datetime), node pulls on wake
- **Actuator commands**: real-time valve on/off via `MSG_TYPE_ACTUATOR_CMD`

### Existing API Endpoints
| Endpoint | Method | Purpose |
|---|---|---|
| `/api/nodes/<device_id>/schedules` | POST | Add/update a schedule entry |
| `/api/nodes/<device_id>/schedules/<index>` | DELETE | Remove a schedule entry |
| `/api/nodes/<device_id>/wake-interval` | POST | Set periodic wake interval |
| `/api/nodes/<int:address>/curtain` | POST | Curtain control (reference pattern) |

### Missing API Endpoints
| Endpoint | Method | Purpose |
|---|---|---|
| `/api/nodes/<device_id>/valve` | POST | Manual valve on/off (best effort) |
| `/api/nodes/<device_id>/schedules` | GET | List current schedules (need to store server-side) |

### Existing UI Patterns
- `CurtainControl.tsx` — actuator control card with action buttons + event history
- `NodeDetail.tsx` — 3-column layout: left sidebar (stats, info, status, advanced), right 2-col (actuator-specific + sensor data)
- Node type detection via `NodeType.IRRIGATION` already in types

## Design

### Page Layout (Irrigation Node Detail)

Same structure as existing NodeDetail, but the right 2-col area shows **IrrigationControl** instead of sensor charts when `node.type === NodeType.IRRIGATION`.

```
┌─────────────────────────────────────────────────┐
│ ← Back to nodes                                 │
│ Node Name                          [Refresh]    │
│ ● Online  IRRIGATION                            │
├──────────────┬──────────────────────────────────┤
│ Node Info    │  ┌─ Run Once ───────────────────┐ │
│ (name/zone)  │  │ Valve 1  [5m][15m][30m][__] │ │
│              │  │                       [Run] │ │
│ Node Status  │  │ Valve 2  [5m][15m][30m][__] │ │
│ (battery,    │  │                       [Run] │ │
│  signal,     │  │ ⚠ Node must be awake        │ │
│              │  └─────────────────────────────┘ │
│  uptime)     │                                  │
│              │  ┌─ Schedules ─────────────────┐ │
│ Advanced     │  │ ┌──────────────────────────┐│ │
│ (reboot,     │  │ │ Valve 1 · 06:00 · 15min ││ │
│  delete)     │  │ │ Mon-Fri      [Edit][Del] ││ │
│              │  │ ├──────────────────────────┤│ │
│              │  │ │ Valve 2 · 18:00 · 30min ││ │
│              │  │ │ Every day    [Edit][Del] ││ │
│              │  │ └──────────────────────────┘│ │
│              │  │                              │ │
│              │  │ [+ Add Schedule]             │ │
│              │  └─────────────────────────────┘ │
│              │                                  │
│              │  ┌─ Recent Events ─────────────┐ │
│              │  │ Valve 1 opened    Apr 4 6:00│ │
│              │  │ Valve 1 closed    Apr 4 6:15│ │
│              │  └─────────────────────────────┘ │
└──────────────┴──────────────────────────────────┘
```

### Component: `IrrigationControl`

**Props**: `{ address: number, deviceId: string }`

Three sections, each in its own card:

#### 1. Run Once (Manual Watering)
- Two rows, one per valve
- Each row: valve label, duration picker (preset buttons: 5m, 15m, 30m, or custom minutes), [Run] button
- When triggered, sends a "run once" command: opens the valve, auto-closes after the specified duration
- Info note: "Best-effort — the node must be awake to receive the command"
- No raw on/off — eliminates the risk of forgetting a valve open
- API: `POST /api/nodes/<device_id>/valve` with `{ valve: 0|1, duration_seconds: 900 }`
- The auto-off is handled by the PMU scheduled wake: hub queues a SET_SCHEDULE with a one-shot entry (or the firmware handles a timed valve command directly)

#### 2. Schedules
- List of active schedules, fetched from server-side storage
- Each entry shows: valve, time (HH:MM), duration (human-readable), days of week, edit/delete buttons
- "Add Schedule" button opens an **inline form** (not a modal — keeps it simple)
- Schedule form fields:
  - Valve: dropdown (Valve 1 / Valve 2)
  - Time: hour + minute inputs
  - Duration: number input in minutes (converted to seconds for API)
  - Days: 7 toggle buttons (M T W T F S S)
- API: existing `POST /schedules` and `DELETE /schedules/<index>`
- **Server-side schedule storage**: schedules are fire-and-forget (queued for node delivery). We need a `GET /schedules` endpoint that returns the *intended* schedule state. Store schedule entries in the database when POST is called, remove on DELETE.

#### 3. Recent Events
- Same pattern as CurtainControl event history
- Scrollable table with event name + timestamp
- Uses existing `GET /api/nodes/<device_id>/events`

### New Event Codes
Add to `EVENT_CODE_NAMES` in types:
```
0x0200: 'Valve 1 Opened'
0x0201: 'Valve 1 Closed'
0x0202: 'Valve 2 Opened'
0x0203: 'Valve 2 Closed'
```

### API Changes Needed

#### 1. `POST /api/nodes/<device_id>/valve` (new)
Send a "run once" valve command with auto-off after specified duration.

```python
@app.route('/api/nodes/<int:device_id>/valve', methods=['POST'])
def run_valve_once(device_id):
    # body: { "valve": 0, "duration_seconds": 900 }
    # Implementation options:
    # a) Queue a one-shot PMU schedule entry (hub sets schedule, PMU wakes node and runs it)
    # b) Send actuator CMD_TURN_ON + queue a delayed CMD_TURN_OFF (fragile if node sleeps)
    # Recommend (a) — PMU handles the timing reliably even through sleep cycles
```

#### 2. `GET /api/nodes/<device_id>/schedules` (new)
Return the intended schedule state from local DB. Needed because schedules are queued asynchronously — we can't query the node directly.

#### 3. Schedule persistence table (new)
Store schedule entries when POST'd, remove on DELETE. Schema:
```sql
CREATE TABLE irrigation_schedules (
    device_id TEXT NOT NULL,
    index INTEGER NOT NULL,
    hour INTEGER NOT NULL,
    minute INTEGER NOT NULL,
    duration INTEGER NOT NULL,
    days INTEGER NOT NULL,
    valve INTEGER NOT NULL,
    created_at INTEGER NOT NULL,
    PRIMARY KEY (device_id, index)
);
```

### Integration into NodeDetail.tsx

In the right 2-col area, add:
```tsx
{node.type === NodeType.IRRIGATION && (
  <IrrigationControl address={node.address} deviceId={node.device_id} />
)}
```

Same pattern as the existing greenhouse/curtain conditional.

## Implementation Order

1. **API**: Add valve control endpoint, schedule persistence table, GET schedules endpoint
2. **Dashboard types**: Add irrigation event codes, schedule types, API client methods
3. **IrrigationControl component**: Valve control card, schedule list/form, event history
4. **NodeDetail integration**: Wire up IrrigationControl for IRRIGATION nodes
5. **Build & test**: `npx tsc --noEmit` for type checking

## Open Questions

- Should schedules show a "pending delivery" status? (The node may not have received the schedule yet if it's sleeping.) **Recommendation**: yes, show a subtle "queued" badge if the schedule was just created and the node hasn't pulled updates yet.
- Do we need a "stop" button for an in-progress run-once? **Recommendation**: yes, show a "Stop" button when a run-once is active (sends CMD_TURN_OFF). Best-effort since node must be awake.
- How does "run once" interact with the PMU sleep cycle? If the node is asleep, the command can't reach it. Options: (a) queue a one-shot schedule via PMU so the node wakes at the right time, or (b) only allow run-once when node is online. Recommend starting with (b) for simplicity, with a clear "node is offline" disabled state.
