# API Loading Performance Optimization Plan

## Overview

Optimize the sensor data API to reduce load times when viewing the 24-hour chart by addressing these bottlenecks:
1. Statistics query scanning all historical data
2. No server-side downsampling (sending up to 5000 points)
3. No HTTP response compression
4. Redundant fields in JSON responses

## Current State

- **Statistics endpoint**: Scans entire node history for min/max/avg
- **Sensor data endpoint**: Returns up to 5000 raw readings with redundant fields
- **No compression**: Large JSON payloads sent uncompressed
- **Frontend**: Already makes concurrent requests for stats and chart data (`Promise.all`)

## Changes

### 1. Add Time Filters to Statistics Query

**Files:** `api/database.py`, `api/app.py`

**Current behavior (`database.py:454-466`):**
```python
SELECT MIN(temperature_centidegrees), MAX(...), AVG(...)
FROM sensor_readings
WHERE node_address = ?  # No time filter
```

**New behavior:**
- Add optional `start_time` and `end_time` parameters to `get_node_statistics()`
- Pass time range from API endpoint to database method
- Update SQL query to filter by time range when provided

**API change:**
```
GET /api/nodes/{addr}/statistics?start_time=X&end_time=Y
```

**Database method signature:**
```python
def get_node_statistics(
    self,
    node_address: int,
    start_time: Optional[int] = None,
    end_time: Optional[int] = None
) -> Optional[dict]
```

---

### 2. Add Server-Side Downsampling

**Files:** `api/database.py`, `api/app.py`

**Goal:** Return at most ~500-800 points regardless of time range, which is sufficient for chart display.

**Approach:** Use LTTB (Largest-Triangle-Three-Buckets) algorithm or simple bucket averaging.

For simplicity, implement **bucket averaging**:
- Divide time range into N buckets (e.g., 500)
- For each bucket, return average temperature/humidity and bucket midpoint timestamp
- This preserves trends while dramatically reducing payload size

**New endpoint parameter:**
```
GET /api/nodes/{addr}/sensor-data?start_time=X&end_time=Y&downsample=500
```

When `downsample` is specified:
- Calculate bucket size: `(end_time - start_time) / downsample`
- Group readings into buckets by timestamp
- Return one averaged point per bucket

**SQL approach (DuckDB supports window functions):**
```sql
SELECT
    node_address,
    (timestamp / bucket_size) * bucket_size as bucket_timestamp,
    AVG(temperature_centidegrees) as temperature_centidegrees,
    AVG(humidity_centipercent) as humidity_centipercent,
    COUNT(*) as sample_count
FROM sensor_readings
WHERE node_address = ? AND timestamp >= ? AND timestamp <= ?
GROUP BY node_address, bucket_timestamp
ORDER BY bucket_timestamp
```

**Response format for downsampled data:**
```json
{
  "node_address": 42,
  "count": 500,
  "downsampled": true,
  "bucket_seconds": 172,
  "readings": [
    {
      "timestamp": 1705507200,
      "temperature_celsius": 22.5,
      "humidity_percent": 65.0,
      "sample_count": 3
    }
  ]
}
```

---

### 3. Enable Gzip Compression

**Files:** `api/app.py`, `api/requirements.txt` (if needed)

**Approach:** Use Flask-Compress middleware

**Installation:**
```bash
pip install flask-compress
```

**Implementation:**
```python
from flask_compress import Compress

app = Flask(__name__)
Compress(app)  # Automatically compresses responses > 500 bytes
```

**Configuration (optional):**
```python
app.config['COMPRESS_MIMETYPES'] = ['application/json']
app.config['COMPRESS_MIN_SIZE'] = 500
```

This provides automatic gzip/deflate compression for all JSON responses.

---

### 4. Remove Redundant Fields from JSON Response

**Files:** `api/database.py`

**Current `to_dict()` output:**
```python
{
    'node_address': ...,
    'device_id': ...,
    'timestamp': ...,
    'temperature_celsius': ...,    # Derived
    'humidity_percent': ...,       # Derived
    'temperature_raw': ...,        # Redundant - remove
    'humidity_raw': ...,           # Redundant - remove
    'flags': ...,
    'received_at': ...
}
```

**New output:**
```python
{
    'timestamp': ...,
    'temperature_celsius': ...,
    'humidity_percent': ...,
    'flags': ...,
}
```

For chart data specifically, we can remove:
- `node_address` - already known from the request
- `device_id` - not needed for charts
- `temperature_raw` / `humidity_raw` - redundant
- `received_at` - not needed for charts

**Approach:** Add a `compact` parameter or create a separate `to_chart_dict()` method:
```python
def to_chart_dict(self) -> dict:
    """Minimal representation for chart display."""
    return {
        'timestamp': self.timestamp,
        'temperature_celsius': self.temperature_celsius,
        'humidity_percent': self.humidity_percent,
    }
```

Use this for the sensor-data endpoint when used for charts.

---

### 5. Verify Concurrent Requests (Frontend)

**Files:** `dashboard/src/components/NodeDetail.tsx`

**Current implementation (already correct):**
```typescript
const [sensorData, stats] = await Promise.all([
  getNodeSensorData(node.address, { startTime, endTime, limit: 5000 }),
  getNodeStatistics(node.address)
]);
```

**Update needed:** Pass time range to statistics call:
```typescript
const [sensorData, stats] = await Promise.all([
  getNodeSensorData(node.address, { startTime, endTime, downsample: 500 }),
  getNodeStatistics(node.address, { startTime, endTime })
]);
```

**Update `api/client.ts`:**
- Add `downsample` parameter to `getNodeSensorData()`
- Add `startTime`/`endTime` parameters to `getNodeStatistics()`

---

## Implementation Order

1. **Enable gzip compression** - Quickest win, no API changes
2. **Add time filters to statistics** - Backend then frontend
3. **Remove redundant fields** - Backend only, reduces payload ~40%
4. **Add downsampling** - Most complex, biggest impact

## Testing

- Verify chart still renders correctly after each change
- Compare payload sizes before/after (use browser DevTools Network tab)
- Measure load time improvements
- Ensure statistics match expected values for time-filtered queries

## Expected Improvements

| Change | Payload Reduction | Notes |
|--------|------------------|-------|
| Gzip compression | ~70-80% | Automatic for JSON |
| Remove redundant fields | ~40% | Fewer bytes per reading |
| Downsampling (5000→500) | ~90% | 10x fewer readings |
| **Combined** | **~97%** | From ~1.2MB to ~30-40KB |

Time-filtered statistics will also be faster as it scans less data.

## Risks

- **Downsampling**: May hide brief spikes/dips in data. Mitigate by keeping raw export available.
- **Compact format**: Breaking change for any external API consumers. Consider versioning or opt-in via query param.
