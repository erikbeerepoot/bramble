# DuckDB Migration Plan

## Overview

This plan outlines the migration from SQLite to DuckDB for the sensor data API. DuckDB is an embedded analytical database that excels at OLAP workloads, making it well-suited for time-series sensor data with analytical queries.

## Current State

### SQLite Implementation (`api/database.py`)
- **Tables**: `sensor_readings` (time-series data), `nodes` (node metadata)
- **Operations**:
  - Inserts: Single and batch sensor readings
  - Queries: Filtered reads with pagination, latest readings
  - Analytics: MIN/MAX/AVG statistics per node
  - Export: CSV generation
- **Access Pattern**: Single-writer (Flask app), append-heavy workload
- **Data Volume**: Relatively small (sensor readings every 30s per node)

## Why DuckDB?

### Advantages
1. **Analytical Performance**: DuckDB is optimized for OLAP queries (aggregations, statistics) which matches our `/statistics` endpoint needs
2. **Columnar Storage**: More efficient for time-series data where queries often filter/aggregate specific columns
3. **Modern SQL**: Window functions, CTEs, and advanced aggregations built-in
4. **Direct Pandas/Arrow Integration**: Future-proofs for data science workflows
5. **Embedded**: No server process needed (like SQLite)
6. **In-process**: Low latency, no network overhead

### Considerations
1. **Write Performance**: DuckDB is optimized for bulk loads, not single-row OLTP inserts - need to batch writes
2. **Concurrency**: Single-writer model (similar to SQLite)
3. **File Size**: Columnar format may have overhead for very small datasets
4. **Maturity**: Newer than SQLite, though production-ready

## Migration Strategy

### Phase 1: Abstraction Layer
Create a database abstraction to support both backends during transition.

```
api/
├── database/
│   ├── __init__.py          # Export public interfaces
│   ├── base.py              # Abstract base class
│   ├── sqlite_backend.py    # Current implementation (renamed)
│   ├── duckdb_backend.py    # New DuckDB implementation
│   └── models.py            # SensorReading dataclass (shared)
```

**Tasks:**
- [ ] Extract `SensorReading` dataclass to `models.py`
- [ ] Create abstract `DatabaseBackend` base class with interface
- [ ] Refactor current SQLite code into `sqlite_backend.py`
- [ ] Add backend selection via config (`DB_BACKEND=sqlite|duckdb`)

### Phase 2: DuckDB Implementation

**Schema Translation:**
```sql
-- DuckDB schema
CREATE TABLE sensor_readings (
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

CREATE TABLE nodes (
    address INTEGER PRIMARY KEY,
    node_type VARCHAR NOT NULL,
    first_seen_at INTEGER NOT NULL,
    last_seen_at INTEGER NOT NULL,
    total_readings INTEGER DEFAULT 0
);
```

**Key Differences from SQLite:**
- No `AUTOINCREMENT` - use `SEQUENCE` or generate IDs
- `TEXT` becomes `VARCHAR`
- Index creation syntax identical
- `INSERT OR IGNORE` becomes `INSERT ... ON CONFLICT DO NOTHING`

**Tasks:**
- [ ] Implement `DuckDBBackend` class
- [ ] Handle connection management (DuckDB connection patterns)
- [ ] Implement batch insert with `executemany()` or `INSERT INTO ... SELECT`
- [ ] Adapt upsert syntax for node stats
- [ ] Add DuckDB-specific optimizations for statistics queries

### Phase 3: Write Batching Strategy

DuckDB performs best with bulk operations. Implement a write buffer:

```python
class WriteBuffer:
    """Buffer sensor readings for batch insertion."""

    def __init__(self, max_size: int = 100, flush_interval: float = 5.0):
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
        """Flush if interval elapsed."""
        if time.time() - self.last_flush >= self.flush_interval:
            self._flush()
```

**Tasks:**
- [ ] Implement `WriteBuffer` class with thread-safe batching
- [ ] Add periodic flush mechanism (background thread or scheduled task)
- [ ] Ensure graceful shutdown flushes pending writes
- [ ] Add config options: `DB_BATCH_SIZE`, `DB_FLUSH_INTERVAL`

### Phase 4: Analytics Enhancements

Leverage DuckDB's analytical capabilities for new features:

**Time-windowed Aggregations:**
```sql
-- Hourly averages (new capability)
SELECT
    node_address,
    time_bucket(INTERVAL '1 hour', to_timestamp(timestamp)) as hour,
    AVG(temperature_centidegrees) / 100.0 as avg_temp,
    AVG(humidity_centipercent) / 100.0 as avg_humidity
FROM sensor_readings
WHERE timestamp >= ? AND timestamp <= ?
GROUP BY node_address, hour
ORDER BY hour DESC;
```

**Tasks:**
- [ ] Add `/api/nodes/<addr>/statistics/hourly` endpoint
- [ ] Add `/api/nodes/<addr>/statistics/daily` endpoint
- [ ] Implement percentile calculations (P50, P95, P99)
- [ ] Add data quality metrics (gaps, outliers)

### Phase 5: Data Migration

**Migration Script:**
```python
def migrate_sqlite_to_duckdb(sqlite_path: str, duckdb_path: str) -> None:
    """Migrate existing SQLite data to DuckDB."""
    import duckdb
    import sqlite3

    # DuckDB can read SQLite directly!
    conn = duckdb.connect(duckdb_path)
    conn.execute(f"INSTALL sqlite; LOAD sqlite;")

    # Copy tables
    conn.execute(f"""
        CREATE TABLE sensor_readings AS
        SELECT * FROM sqlite_scan('{sqlite_path}', 'sensor_readings')
    """)

    conn.execute(f"""
        CREATE TABLE nodes AS
        SELECT * FROM sqlite_scan('{sqlite_path}', 'nodes')
    """)

    # Add indexes and constraints
    conn.execute("CREATE UNIQUE INDEX idx_node_ts ON sensor_readings(node_address, timestamp)")
    conn.execute("CREATE INDEX idx_timestamp ON sensor_readings(timestamp)")

    conn.close()
```

**Tasks:**
- [ ] Create migration script with progress reporting
- [ ] Add data validation (row counts, checksums)
- [ ] Create rollback script (DuckDB to SQLite export)
- [ ] Document migration procedure

### Phase 6: Testing and Validation

**Test Coverage:**
- [ ] Unit tests for `DuckDBBackend` matching existing SQLite tests
- [ ] Integration tests for write buffer behavior
- [ ] Performance benchmarks (insert rate, query latency, statistics)
- [ ] Concurrency tests (simultaneous reads during batch writes)
- [ ] Migration script tests with sample data

**Validation Checklist:**
- [ ] All existing API endpoints return identical results
- [ ] Insert performance meets requirements (handle 30s sensor intervals)
- [ ] Statistics queries faster or equivalent
- [ ] Memory usage acceptable on Raspberry Pi Zero 2 W

## Implementation Order

1. **Create abstraction layer** - No functionality change, just refactoring
2. **Implement DuckDB backend** - Feature parity with SQLite
3. **Add write batching** - Required for good DuckDB write performance
4. **Migration tooling** - Scripts to move existing data
5. **Testing and benchmarks** - Validate on target hardware
6. **Switch default backend** - Change config default to DuckDB
7. **Deprecate SQLite backend** - After confidence period

## Configuration

```python
# config.py additions
class Config:
    # Database backend selection
    DB_BACKEND = os.getenv('DB_BACKEND', 'sqlite')  # 'sqlite' or 'duckdb'

    # DuckDB specific
    DUCKDB_PATH = os.getenv('DUCKDB_PATH', '/data/sensor_data.duckdb')

    # Write batching
    DB_BATCH_SIZE = int(os.getenv('DB_BATCH_SIZE', '100'))
    DB_FLUSH_INTERVAL = float(os.getenv('DB_FLUSH_INTERVAL', '5.0'))
```

## File Changes Summary

| File | Change |
|------|--------|
| `api/database.py` | Refactor to `api/database/` package |
| `api/database/__init__.py` | New: exports and factory function |
| `api/database/base.py` | New: abstract interface |
| `api/database/models.py` | New: shared data classes |
| `api/database/sqlite_backend.py` | Moved: current implementation |
| `api/database/duckdb_backend.py` | New: DuckDB implementation |
| `api/database/write_buffer.py` | New: batch write support |
| `api/database/migration.py` | New: SQLite to DuckDB migration |
| `api/config.py` | Update: new configuration options |
| `api/requirements.txt` | Update: add `duckdb` dependency |
| `api/tests/test_database.py` | Update: test both backends |

## Rollback Plan

1. Data can be exported from DuckDB to SQLite using DuckDB's built-in export
2. Keep SQLite backend code until migration is validated in production
3. Config switch allows instant rollback: `DB_BACKEND=sqlite`
4. Maintain SQLite database file during transition period

## Risks and Mitigations

| Risk | Impact | Mitigation |
|------|--------|------------|
| DuckDB write latency | Missed sensor readings | Write buffer with async flush |
| Memory usage on RPi Zero | OOM crashes | Configure DuckDB memory limits |
| Migration data loss | Historical data lost | Validation checksums, keep SQLite backup |
| API incompatibility | Breaking changes | Comprehensive integration tests |
| DuckDB library size | SD card space | Acceptable (~40MB) |

## Success Criteria

- [ ] All existing API tests pass with DuckDB backend
- [ ] Statistics queries 2x faster (benchmark on RPi)
- [ ] Write batching handles burst of 100 readings without data loss
- [ ] Memory usage under 128MB on Raspberry Pi Zero 2 W
- [ ] Migration completes successfully with data integrity verified

## Dependencies

```
# Add to requirements.txt
duckdb>=0.10.0
```

## References

- [DuckDB Python API](https://duckdb.org/docs/api/python/overview)
- [DuckDB SQLite Scanner](https://duckdb.org/docs/extensions/sqlite_scanner)
- [Time Series in DuckDB](https://duckdb.org/docs/guides/sql_features/timestamps)
