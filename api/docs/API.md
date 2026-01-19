# Bramble API Reference

REST API for managing irrigation schedules and sensor nodes over LoRa.

## Base URL

```
http://localhost:5000/api
```

## Authentication

No authentication required (intended for local network use).

---

## Health & System

### Health Check

**GET** `/api/health`

```json
{
  "status": "healthy",
  "serial_connected": true,
  "serial_port": "/dev/ttyACM0"
}
```

### System Time

**GET** `/api/system/time`

```json
{
  "datetime": "2025-01-15T14:30:00",
  "formatted": "2025-01-15 14:30:00",
  "weekday": 3,
  "timestamp": 1736953800
}
```

---

## Nodes

### List Nodes

**GET** `/api/nodes`

```json
{
  "count": 2,
  "nodes": [
    {
      "address": 42,
      "device_id": 1234567890,
      "type": "IRRIGATION",
      "online": true,
      "last_seen_seconds": 15,
      "metadata": {
        "address": 42,
        "name": "Garden Valve",
        "location": "Backyard",
        "notes": null,
        "zone_id": 1,
        "updated_at": 1736953800
      }
    }
  ]
}
```

### Get Node

**GET** `/api/nodes/{addr}`

```json
{
  "address": 42,
  "device_id": 1234567890,
  "type": "IRRIGATION",
  "online": true,
  "last_seen_seconds": 15
}
```

### Get Node Metadata

**GET** `/api/nodes/{addr}/metadata`

```json
{
  "address": 42,
  "name": "Garden Valve",
  "location": "Backyard",
  "notes": "Replaced solenoid 2024-06",
  "zone_id": 1,
  "updated_at": 1736953800
}
```

### Update Node Metadata

**PUT** `/api/nodes/{addr}/metadata`

**Request:**
```json
{
  "name": "Garden Valve",
  "location": "Backyard",
  "notes": "Replaced solenoid 2024-06"
}
```

**Response:** Updated metadata object.

### Set Node Zone

**PUT** `/api/nodes/{addr}/zone`

Assign a node to a zone or remove from zone.

**Request:**
```json
{
  "zone_id": 1
}
```

Or to remove from zone:
```json
{
  "zone_id": null
}
```

**Response:** Updated metadata object.

### Get Update Queue

**GET** `/api/nodes/{addr}/queue`

```json
{
  "node_address": 42,
  "count": 2,
  "updates": [
    {
      "sequence": 5,
      "type": "SET_SCHEDULE",
      "age_seconds": 30
    }
  ]
}
```

---

## Zones

Zones allow organizing nodes into logical groups with color-coded UI indicators.

### List Zones

**GET** `/api/zones`

```json
{
  "count": 2,
  "zones": [
    {
      "id": 1,
      "name": "Greenhouse",
      "color": "#4CAF50",
      "description": "North greenhouse section"
    },
    {
      "id": 2,
      "name": "Garden",
      "color": "#2196F3",
      "description": null
    }
  ]
}
```

### Create Zone

**POST** `/api/zones`

**Request:**
```json
{
  "name": "Greenhouse",
  "color": "#4CAF50",
  "description": "North greenhouse section"
}
```

- `name` (required): Zone name
- `color` (required): Hex color code (e.g., `#4CAF50`)
- `description` (optional): Zone description

**Response (201 Created):**
```json
{
  "id": 1,
  "name": "Greenhouse",
  "color": "#4CAF50",
  "description": "North greenhouse section"
}
```

### Get Zone

**GET** `/api/zones/{id}`

```json
{
  "id": 1,
  "name": "Greenhouse",
  "color": "#4CAF50",
  "description": "North greenhouse section"
}
```

### Update Zone

**PUT** `/api/zones/{id}`

**Request:**
```json
{
  "name": "Updated Name",
  "color": "#FF5722",
  "description": "Updated description"
}
```

All fields are optional; only provided fields are updated.

**Response:** Updated zone object.

### Delete Zone

**DELETE** `/api/zones/{id}`

Deletes a zone. Nodes assigned to this zone become unzoned.

**Response:**
```json
{
  "message": "Zone 1 deleted"
}
```

---

## Schedules

### Add Schedule

**POST** `/api/nodes/{addr}/schedules`

**Request:**
```json
{
  "index": 0,
  "hour": 14,
  "minute": 30,
  "duration": 900,
  "days": 127,
  "valve": 0
}
```

**Parameters:**
- `index`: Schedule slot (0-7)
- `hour`: Hour (0-23)
- `minute`: Minute (0-59)
- `duration`: Duration in seconds (0-65535)
- `days`: Day bitmask (127 = all days, 1 = Sunday, 2 = Monday, 4 = Tuesday, etc.)
- `valve`: Valve ID (0+)

**Response (202 Accepted):**
```json
{
  "status": "queued",
  "task_id": "abc123",
  "node_address": 42,
  "schedule": { ... },
  "message": "Command queued for delivery"
}
```

### Remove Schedule

**DELETE** `/api/nodes/{addr}/schedules/{index}`

**Response (202 Accepted):**
```json
{
  "status": "queued",
  "task_id": "def456",
  "node_address": 42,
  "schedule_index": 0,
  "message": "Command queued for delivery"
}
```

---

## Node Configuration

### Set Wake Interval

**POST** `/api/nodes/{addr}/wake-interval`

**Request:**
```json
{
  "interval_seconds": 60
}
```

- `interval_seconds`: Wake interval in seconds (10-3600)

**Response (202 Accepted):**
```json
{
  "status": "queued",
  "task_id": "ghi789",
  "node_address": 42,
  "interval_seconds": 60,
  "message": "Command queued for delivery"
}
```

### Set DateTime

**POST** `/api/nodes/{addr}/datetime`

**Request:**
```json
{
  "year": 25,
  "month": 1,
  "day": 15,
  "weekday": 3,
  "hour": 14,
  "minute": 30,
  "second": 0
}
```

**Response (202 Accepted):**
```json
{
  "status": "queued",
  "task_id": "jkl012",
  "node_address": 42,
  "datetime": { ... },
  "message": "Command queued for delivery"
}
```

---

## Tasks

Commands to nodes are queued and processed asynchronously. Use task endpoints to check status.

### Check Task Status

**GET** `/api/tasks/{task_id}`

**Response (pending):**
```json
{
  "status": "pending",
  "task_id": "abc123"
}
```

**Response (completed):**
```json
{
  "status": "completed",
  "task_id": "abc123",
  "result": {
    "status": "success",
    "response": ["QUEUED SET_SCHEDULE 42 1"]
  }
}
```

**Response (failed):**
```json
{
  "status": "failed",
  "task_id": "abc123",
  "error": "Hub timeout"
}
```

---

## Sensor Data

### Query Sensor Data

**GET** `/api/sensor-data`

**Query Parameters:**
- `node_address` (optional): Filter by node
- `start_time` (optional): Unix timestamp
- `end_time` (optional): Unix timestamp
- `limit` (default 1000, max 10000): Maximum records
- `offset` (default 0): Pagination offset

**Response:**
```json
{
  "count": 100,
  "total": 5000,
  "limit": 100,
  "offset": 0,
  "readings": [
    {
      "node_address": 42,
      "device_id": 1234567890,
      "timestamp": 1736953800,
      "temperature_celsius": 22.5,
      "humidity_percent": 65.0,
      "temperature_raw": 2250,
      "humidity_raw": 6500,
      "flags": 0,
      "received_at": 1736953805
    }
  ]
}
```

### Get Node Sensor Data

**GET** `/api/nodes/{addr}/sensor-data`

Same query parameters as above, filtered to specific node.

### Get Latest Reading

**GET** `/api/nodes/{addr}/sensor-data/latest`

Returns the most recent sensor reading for a node.

### Get Node Statistics

**GET** `/api/nodes/{addr}/statistics`

```json
{
  "address": 42,
  "device_id": 1234567890,
  "node_type": "SENSOR",
  "first_seen_at": 1736000000,
  "last_seen_at": 1736953800,
  "total_readings": 5000,
  "temperature": {
    "min_celsius": 15.2,
    "max_celsius": 32.1,
    "avg_celsius": 22.5
  },
  "humidity": {
    "min_percent": 40.0,
    "max_percent": 85.0,
    "avg_percent": 62.5
  }
}
```

### Export Sensor Data

**GET** `/api/sensor-data/export`

**Query Parameters:**
- `node_address` (optional): Filter by node
- `start_time` (optional): Unix timestamp
- `end_time` (optional): Unix timestamp
- `format` (default `csv`): Export format

Returns CSV file download.

---

## Usage Examples

```bash
# List all nodes
curl http://localhost:5000/api/nodes

# Create a zone
curl -X POST http://localhost:5000/api/zones \
  -H "Content-Type: application/json" \
  -d '{"name": "Greenhouse", "color": "#4CAF50"}'

# Assign node to zone
curl -X PUT http://localhost:5000/api/nodes/42/zone \
  -H "Content-Type: application/json" \
  -d '{"zone_id": 1}'

# Add irrigation schedule (water at 2:30 PM for 15 minutes, all days)
curl -X POST http://localhost:5000/api/nodes/42/schedules \
  -H "Content-Type: application/json" \
  -d '{
    "index": 0,
    "hour": 14,
    "minute": 30,
    "duration": 900,
    "days": 127,
    "valve": 0
  }'

# Check task status
curl http://localhost:5000/api/tasks/abc123

# Remove schedule
curl -X DELETE http://localhost:5000/api/nodes/42/schedules/0

# Set wake interval to 1 minute
curl -X POST http://localhost:5000/api/nodes/42/wake-interval \
  -H "Content-Type: application/json" \
  -d '{"interval_seconds": 60}'

# Get sensor data for last 24 hours
curl "http://localhost:5000/api/nodes/42/sensor-data?start_time=$(($(date +%s) - 86400))"

# Check node's update queue
curl http://localhost:5000/api/nodes/42/queue
```
