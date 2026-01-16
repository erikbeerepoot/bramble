# DuckDB Migration Plan

## Overview

Replace SQLite with DuckDB in `api/database.py`. This is a straightforward swap - no data migration needed.

## Why DuckDB?

- **Analytical queries**: Better performance for aggregations (statistics endpoints)
- **Columnar storage**: Efficient for time-series sensor data
- **Embedded**: No server process, same deployment model as SQLite

## Key Difference: Write Batching

DuckDB is optimized for bulk loads, not single-row inserts. Add a write buffer to batch sensor readings before flushing to disk.

## Implementation

### 1. Update `database.py`

Replace `sqlite3` with `duckdb`. Key syntax changes:

| SQLite | DuckDB |
|--------|--------|
| `sqlite3.connect()` | `duckdb.connect()` |
| `INSERT OR IGNORE` | `INSERT ... ON CONFLICT DO NOTHING` |
| `INTEGER PRIMARY KEY AUTOINCREMENT` | `INTEGER PRIMARY KEY` (use sequence if needed) |
| `TEXT` | `VARCHAR` |

**Schema:**
```sql
CREATE TABLE IF NOT EXISTS sensor_readings (
    id INTEGER PRIMARY KEY,
    node_address INTEGER NOT NULL,
    timestamp INTEGER NOT NULL,
    temperature_centidegrees INTEGER NOT NULL,
    humidity_centipercent INTEGER NOT NULL,
    flags INTEGER DEFAULT 0,
    received_at INTEGER NOT NULL,
    created_at TIMESTAMP DEFAULT current_timestamp,
    UNIQUE(node_address, timestamp)
);

CREATE TABLE IF NOT EXISTS nodes (
    address INTEGER PRIMARY KEY,
    node_type VARCHAR NOT NULL,
    first_seen_at INTEGER NOT NULL,
    last_seen_at INTEGER NOT NULL,
    total_readings INTEGER DEFAULT 0
);

CREATE INDEX IF NOT EXISTS idx_node_timestamp ON sensor_readings(node_address, timestamp);
CREATE INDEX IF NOT EXISTS idx_timestamp ON sensor_readings(timestamp);
CREATE INDEX IF NOT EXISTS idx_last_seen ON nodes(last_seen_at);
```

### 2. Add Write Buffer

```python
class WriteBuffer:
    """Buffer sensor readings for batch insertion."""

    def __init__(self, database: 'SensorDatabase', max_size: int = 100, flush_interval: float = 5.0):
        self.database = database
        self.buffer: list[SensorReading] = []
        self.max_size = max_size
        self.flush_interval = flush_interval
        self.last_flush = time.time()
        self._lock = threading.Lock()

    def add(self, reading: SensorReading) -> None:
        with self._lock:
            self.buffer.append(reading)
            if len(self.buffer) >= self.max_size:
                self._flush()

    def maybe_flush(self) -> None:
        """Call periodically to flush on interval."""
        with self._lock:
            if self.buffer and (time.time() - self.last_flush >= self.flush_interval):
                self._flush()

    def _flush(self) -> None:
        if not self.buffer:
            return
        self.database.insert_batch(self.buffer)
        self.buffer.clear()
        self.last_flush = time.time()

    def shutdown(self) -> None:
        """Flush remaining on shutdown."""
        with self._lock:
            self._flush()
```

### 3. Update Config

```python
# config.py
SENSOR_DB_PATH = os.getenv('SENSOR_DB_PATH', '/data/sensor_data.duckdb')
DB_BATCH_SIZE = int(os.getenv('DB_BATCH_SIZE', '100'))
DB_FLUSH_INTERVAL = float(os.getenv('DB_FLUSH_INTERVAL', '5.0'))
```

### 4. Update Dependencies

```
# requirements.txt
duckdb>=0.10.0
```

## Tasks

- [x] Replace `sqlite3` import with `duckdb`
- [x] Update schema (VARCHAR, remove AUTOINCREMENT)
- [x] Change `INSERT OR IGNORE` to `INSERT ... ON CONFLICT DO NOTHING`
- [x] Add `WriteBuffer` class
- [x] Wire up buffer in `SensorDatabase` for `insert_reading()`
- [ ] Add periodic flush call (in Flask app or background thread) - optional, buffer available via `get_write_buffer()`
- [ ] Add `shutdown()` call on app teardown - optional, for graceful shutdown
- [x] Update config with new defaults
- [x] Add `duckdb` to pyproject.toml
- [x] Update tests to work with DuckDB

## File Changes

| File | Change |
|------|--------|
| `api/database.py` | Replace sqlite3 with duckdb, add WriteBuffer |
| `api/config.py` | Update path default, add batch config |
| `api/requirements.txt` | Add duckdb |
| `api/tests/test_database.py` | Update for DuckDB |
