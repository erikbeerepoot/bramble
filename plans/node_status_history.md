# Node Status History Feature Plan

## Problem

When viewing sensor data, users see gaps but have no way to understand why:
- Was the node offline?
- Did the sensor fail?
- Was the battery dead?
- Were there communication issues?

Currently the system tracks **current** node status but not **history** of status changes.

## Solution

Add a status history table to the API database that records:
- Online/offline transitions
- Error flag changes
- Signal strength and battery at time of event

This allows users to correlate data gaps with node issues.

## Architecture

```
[Hub Firmware]                    [Raspberry Pi API]
     │                                   │
     │  NODE_STATUS <addr> <status>      │
     │  <reason> <error_flags>           │
     │  <signal> <battery>               │
     ├──────────────────────────────────►│
     │                                   │
     │                            ┌──────▼──────┐
     │                            │   DuckDB    │
     │                            │  node_      │
     │                            │  status_    │
     │                            │  history    │
     │                            └─────────────┘
```

## Database Schema

```sql
CREATE TABLE node_status_history (
    id INTEGER PRIMARY KEY,
    node_address INTEGER NOT NULL,
    device_id TEXT,
    timestamp TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    status TEXT NOT NULL,           -- 'online', 'offline', 'error_change'
    reason TEXT,                    -- 'timeout', 'recovered', 'registered', 'deregistered', 'heartbeat'
    error_flags INTEGER DEFAULT 0,  -- Raw bitmask
    signal_strength INTEGER,        -- RSSI in dBm
    battery_level INTEGER           -- Battery percentage
);

CREATE INDEX idx_status_history_node_time
    ON node_status_history(node_address, timestamp);
```

## Error Flags Reference

From `src/lora/message.h`:

| Flag | Value | Description |
|------|-------|-------------|
| ERR_FLAG_NONE | 0x00 | No errors |
| ERR_FLAG_SENSOR_FAILURE | 0x01 | Temp/humidity sensor not responding |
| ERR_FLAG_FLASH_FAILURE | 0x02 | External flash not responding |
| ERR_FLAG_FLASH_FULL | 0x04 | Flash storage >90% full |
| ERR_FLAG_PMU_FAILURE | 0x08 | PMU communication failure |
| ERR_FLAG_BATTERY_LOW | 0x10 | Battery <20% |
| ERR_FLAG_BATTERY_CRITICAL | 0x20 | Battery <10% |
| ERR_FLAG_RTC_NOT_SYNCED | 0x40 | RTC never synchronized |
| ERR_FLAG_TX_FAILURES | 0x80 | Consecutive transmission failures |
| ERR_FLAG_HIGH_TIMEOUTS | 0x100 | High message timeout rate |

## Event Triggers

| Event | Status | Reason | Where Triggered |
|-------|--------|--------|-----------------|
| Node comes online | `online` | `recovered` | `AddressManager::updateLastSeen()` |
| Node times out | `offline` | `timeout` | `AddressManager::checkForInactiveNodes()` |
| Node registered | `online` | `registered` | `AddressManager::registerNode()` |
| Node deregistered | `offline` | `deregistered` | `AddressManager::deregisterInactiveNodes()` |
| Error flags change | `error_change` | `heartbeat` | `HubMode::handleHeartbeat()` |
| Periodic heartbeat (optional) | `online` | `heartbeat` | `HubMode::handleHeartbeat()` |

## Implementation Phases

### Phase 1: Hub Firmware - Serial Protocol (1-2 hours)

**File**: `src/modes/hub_mode.cpp`

Add new serial message when status changes:

```cpp
// Format: NODE_STATUS <addr> <device_id> <status> <reason> <error_flags> <signal> <battery>
printf("NODE_STATUS %u %llu %s %s %u %d %u\n",
       node_address,
       device_id,
       status,      // "online", "offline", "error_change"
       reason,      // "timeout", "recovered", "registered", "deregistered", "heartbeat"
       error_flags,
       signal_strength,
       battery_level);
```

**Modifications needed:**

1. `AddressManager::updateLastSeen()` - Emit `NODE_STATUS online recovered` when `is_active` transitions false→true
2. `AddressManager::checkForInactiveNodes()` - Emit `NODE_STATUS offline timeout` when marking inactive
3. `AddressManager::registerNode()` - Emit `NODE_STATUS online registered`
4. `AddressManager::deregisterInactiveNodes()` - Emit `NODE_STATUS offline deregistered`
5. `HubMode` heartbeat handler - Track last error flags per node, emit `NODE_STATUS error_change heartbeat` when flags change

**New state tracking in HubMode:**
```cpp
// Track last error flags to detect changes
std::map<uint16_t, uint16_t> last_error_flags_;
```

### Phase 2: API - Database Schema (30 min)

**File**: `api/database.py`

Add table creation in `init_db()`:

```python
def init_db():
    # ... existing tables ...

    conn.execute("""
        CREATE TABLE IF NOT EXISTS node_status_history (
            id INTEGER PRIMARY KEY,
            node_address INTEGER NOT NULL,
            device_id TEXT,
            timestamp TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
            status TEXT NOT NULL,
            reason TEXT,
            error_flags INTEGER DEFAULT 0,
            signal_strength INTEGER,
            battery_level INTEGER
        )
    """)

    conn.execute("""
        CREATE INDEX IF NOT EXISTS idx_status_history_node_time
        ON node_status_history(node_address, timestamp)
    """)
```

### Phase 3: API - Serial Handler (1 hour)

**File**: `api/serial_interface.py`

Add handler for `NODE_STATUS` messages:

```python
def _handle_node_status(self, parts: list[str]) -> None:
    """Handle NODE_STATUS messages from hub."""
    # NODE_STATUS <addr> <device_id> <status> <reason> <error_flags> <signal> <battery>
    if len(parts) < 8:
        logger.warning(f"Invalid NODE_STATUS message: {parts}")
        return

    node_address = int(parts[1])
    device_id = parts[2]
    status = parts[3]
    reason = parts[4]
    error_flags = int(parts[5])
    signal_strength = int(parts[6])
    battery_level = int(parts[7])

    # Insert into history
    with get_db_connection() as conn:
        conn.execute("""
            INSERT INTO node_status_history
            (node_address, device_id, status, reason, error_flags, signal_strength, battery_level)
            VALUES (?, ?, ?, ?, ?, ?, ?)
        """, (node_address, device_id, status, reason, error_flags, signal_strength, battery_level))

    logger.info(f"Node {node_address} status: {status} ({reason}), errors=0x{error_flags:04x}")
```

Add to message dispatch in `_process_line()`:

```python
elif parts[0] == "NODE_STATUS":
    self._handle_node_status(parts)
```

### Phase 4: API - REST Endpoints (2 hours)

**File**: `api/app.py`

#### GET /api/nodes/{addr}/status-history

```python
@app.route('/api/nodes/<int:addr>/status-history', methods=['GET'])
def get_node_status_history(addr: int):
    """Get status history for a node."""
    start = request.args.get('start')  # ISO timestamp
    end = request.args.get('end')      # ISO timestamp
    limit = request.args.get('limit', 100, type=int)

    query = """
        SELECT id, node_address, device_id, timestamp, status, reason,
               error_flags, signal_strength, battery_level
        FROM node_status_history
        WHERE node_address = ?
    """
    params = [addr]

    if start:
        query += " AND timestamp >= ?"
        params.append(start)
    if end:
        query += " AND timestamp <= ?"
        params.append(end)

    query += " ORDER BY timestamp DESC LIMIT ?"
    params.append(limit)

    with get_db_connection() as conn:
        rows = conn.execute(query, params).fetchall()

    # Decode error flags
    def decode_flags(flags: int) -> list[str]:
        flag_names = []
        if flags & 0x01: flag_names.append("SENSOR_FAILURE")
        if flags & 0x02: flag_names.append("FLASH_FAILURE")
        if flags & 0x04: flag_names.append("FLASH_FULL")
        if flags & 0x08: flag_names.append("PMU_FAILURE")
        if flags & 0x10: flag_names.append("BATTERY_LOW")
        if flags & 0x20: flag_names.append("BATTERY_CRITICAL")
        if flags & 0x40: flag_names.append("RTC_NOT_SYNCED")
        if flags & 0x80: flag_names.append("TX_FAILURES")
        if flags & 0x100: flag_names.append("HIGH_TIMEOUTS")
        return flag_names

    history = [{
        "id": row[0],
        "node_address": row[1],
        "device_id": row[2],
        "timestamp": row[3].isoformat() if row[3] else None,
        "status": row[4],
        "reason": row[5],
        "error_flags": row[6],
        "errors": decode_flags(row[6] or 0),
        "signal_strength": row[7],
        "battery_level": row[8]
    } for row in rows]

    return jsonify(history)
```

#### GET /api/nodes/{addr}/gaps

Computed endpoint that returns data gaps with likely causes:

```python
@app.route('/api/nodes/<int:addr>/gaps', methods=['GET'])
def get_node_gaps(addr: int):
    """Get data gaps for a node with status context."""
    start = request.args.get('start')
    end = request.args.get('end')
    min_gap_minutes = request.args.get('min_gap', 5, type=int)

    # Get sensor readings to find gaps
    # Get status history for the same period
    # Correlate gaps with offline periods / error events

    # Returns list of gaps with:
    # - gap_start, gap_end, duration_minutes
    # - status_events: list of status changes during gap
    # - likely_cause: "offline", "sensor_failure", "communication", "unknown"

    # ... implementation ...
```

### Phase 5: Dashboard Integration (optional, 2-3 hours)

**File**: `dashboard/src/components/NodeStatusHistory.tsx`

- Timeline visualization showing online/offline periods
- Overlay on sensor data charts to show gaps
- Error flag badges with tooltips
- Filter by status type, date range

## File Checklist

### Hub Firmware
- [ ] `src/modes/hub_mode.h` - Add `last_error_flags_` map
- [ ] `src/modes/hub_mode.cpp` - Emit NODE_STATUS on heartbeat error flag changes
- [ ] `src/lora/address_manager.h` - Add callback interface for status changes
- [ ] `src/lora/address_manager.cpp` - Call callback on status transitions

### API
- [ ] `api/database.py` - Add `node_status_history` table
- [ ] `api/serial_interface.py` - Handle `NODE_STATUS` messages
- [ ] `api/app.py` - Add `/status-history` and `/gaps` endpoints
- [ ] `api/docs/API.md` - Document new endpoints

### Dashboard (optional)
- [ ] `dashboard/src/components/NodeStatusHistory.tsx` - Timeline component
- [ ] `dashboard/src/pages/NodeDetail.tsx` - Integrate status history

## API Response Examples

### GET /api/nodes/1/status-history

```json
[
  {
    "id": 42,
    "node_address": 1,
    "device_id": "123456789",
    "timestamp": "2026-02-04T10:30:00Z",
    "status": "online",
    "reason": "recovered",
    "error_flags": 0,
    "errors": [],
    "signal_strength": -72,
    "battery_level": 85
  },
  {
    "id": 41,
    "node_address": 1,
    "device_id": "123456789",
    "timestamp": "2026-02-04T09:15:00Z",
    "status": "offline",
    "reason": "timeout",
    "error_flags": 1,
    "errors": ["SENSOR_FAILURE"],
    "signal_strength": -95,
    "battery_level": 82
  }
]
```

### GET /api/nodes/1/gaps

```json
[
  {
    "gap_start": "2026-02-04T09:15:00Z",
    "gap_end": "2026-02-04T10:30:00Z",
    "duration_minutes": 75,
    "status_events": [
      {"timestamp": "2026-02-04T09:15:00Z", "status": "offline", "reason": "timeout"}
    ],
    "likely_cause": "offline",
    "error_flags_during_gap": ["SENSOR_FAILURE"]
  }
]
```

## Success Criteria

- [ ] Hub emits `NODE_STATUS` messages on all status transitions
- [ ] API stores status history in DuckDB
- [ ] `/status-history` endpoint returns historical events with decoded error flags
- [ ] `/gaps` endpoint correlates data gaps with status events
- [ ] Can answer "why is there a gap from 9am to 10am?" by checking status history

## Estimated Timeline

| Phase | Effort |
|-------|--------|
| Phase 1: Hub firmware | 1-2 hours |
| Phase 2: Database schema | 30 min |
| Phase 3: Serial handler | 1 hour |
| Phase 4: REST endpoints | 2 hours |
| Phase 5: Dashboard (optional) | 2-3 hours |
| **Total (without dashboard)** | **4-6 hours** |
| **Total (with dashboard)** | **7-9 hours** |

## Future Enhancements

1. **Retention policy** - Auto-delete history older than N days
2. **Aggregation** - Daily/weekly uptime percentages
3. **Alerts** - Notify when node goes offline or error flags appear
4. **Export** - CSV export of status history
