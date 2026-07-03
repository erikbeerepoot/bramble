"""DuckDB database for sensor data storage."""
import duckdb
import json
import logging
import time
import threading
from pathlib import Path
from typing import Optional
from dataclasses import dataclass
from contextlib import contextmanager

from config import Config

logger = logging.getLogger(__name__)


@dataclass
class SensorReading:
    """Represents a single sensor data reading."""
    device_id: int  # Primary identifier (64-bit hardware unique ID)
    timestamp: int  # Unix timestamp in seconds
    temperature_centidegrees: int  # Temperature in 0.01C units
    humidity_centipercent: int  # Humidity in 0.01% units
    flags: int = 0
    received_at: Optional[int] = None  # When hub received it
    address: Optional[int] = None  # LoRa address (for reference)

    @property
    def temperature_celsius(self) -> float:
        """Temperature in degrees Celsius."""
        return self.temperature_centidegrees / 100.0

    @property
    def humidity_percent(self) -> float:
        """Humidity as percentage."""
        return self.humidity_centipercent / 100.0

    def to_dict(self) -> dict:
        """Convert to dictionary for JSON serialization."""
        return {
            'device_id': str(self.device_id),  # String to preserve JS precision
            'address': self.address,
            'timestamp': self.timestamp,
            'temperature_celsius': self.temperature_celsius,
            'humidity_percent': self.humidity_percent,
            'temperature_raw': self.temperature_centidegrees,
            'humidity_raw': self.humidity_centipercent,
            'flags': self.flags,
            'received_at': self.received_at
        }

    def to_chart_dict(self) -> dict:
        """Convert to compact dictionary for chart display."""
        result = {
            'timestamp': self.timestamp,
            'temperature_celsius': self.temperature_celsius,
            'humidity_percent': self.humidity_percent,
        }
        if self.flags:
            result['flags'] = self.flags
        return result


class WriteBuffer:
    """Buffer sensor readings for batch insertion.

    DuckDB performs best with bulk operations, so we buffer
    individual writes and flush them periodically or when
    the buffer is full.
    """

    def __init__(
        self,
        database: 'SensorDatabase',
        max_size: int = Config.DB_BATCH_SIZE,
        flush_interval: float = Config.DB_FLUSH_INTERVAL
    ):
        self.database = database
        self.buffer: list[SensorReading] = []
        self.max_size = max_size
        self.flush_interval = flush_interval
        self.last_flush = time.time()
        self._lock = threading.Lock()

    def add(self, reading: SensorReading) -> None:
        """Add a reading to the buffer, flushing if full."""
        with self._lock:
            self.buffer.append(reading)
            if len(self.buffer) >= self.max_size:
                self._flush()

    def maybe_flush(self) -> None:
        """Flush if interval has elapsed. Call periodically."""
        with self._lock:
            if self.buffer and (time.time() - self.last_flush >= self.flush_interval):
                self._flush()

    def _flush(self) -> None:
        """Internal flush - must hold lock."""
        if not self.buffer:
            return
        try:
            inserted, duplicates = self.database.insert_batch(self.buffer)
            logger.debug(f"WriteBuffer flushed: {inserted} inserted, {duplicates} duplicates")
        except Exception as e:
            logger.error(f"WriteBuffer flush failed: {e}")
        self.buffer.clear()
        self.last_flush = time.time()

    def flush(self) -> None:
        """Force flush the buffer."""
        with self._lock:
            self._flush()

    def shutdown(self) -> None:
        """Flush remaining on shutdown."""
        self.flush()


class SensorDatabase:
    """DuckDB database manager for sensor readings."""

    SCHEMA = """
    CREATE TABLE IF NOT EXISTS sensor_readings (
        id INTEGER PRIMARY KEY,
        device_id UBIGINT NOT NULL,
        address INTEGER,
        timestamp INTEGER NOT NULL,
        temperature_centidegrees INTEGER NOT NULL,
        humidity_centipercent INTEGER NOT NULL,
        flags INTEGER DEFAULT 0,
        received_at INTEGER NOT NULL,
        created_at TIMESTAMP DEFAULT current_timestamp,
        UNIQUE(device_id, timestamp)
    );

    CREATE INDEX IF NOT EXISTS idx_device_timestamp
        ON sensor_readings(device_id, timestamp);

    CREATE INDEX IF NOT EXISTS idx_timestamp
        ON sensor_readings(timestamp);

    CREATE TABLE IF NOT EXISTS nodes (
        device_id UBIGINT PRIMARY KEY,
        address INTEGER,
        node_type VARCHAR NOT NULL,
        first_seen_at INTEGER NOT NULL,
        last_seen_at INTEGER NOT NULL,
        total_readings INTEGER DEFAULT 0,
        valve_count INTEGER
    );

    CREATE INDEX IF NOT EXISTS idx_last_seen
        ON nodes(last_seen_at);

    CREATE INDEX IF NOT EXISTS idx_nodes_address
        ON nodes(address);

    CREATE TABLE IF NOT EXISTS node_metadata (
        device_id UBIGINT PRIMARY KEY,
        name VARCHAR,
        notes VARCHAR,
        zone_id INTEGER,
        updated_at INTEGER NOT NULL
    );

    CREATE TABLE IF NOT EXISTS zones (
        id INTEGER PRIMARY KEY,
        name VARCHAR NOT NULL,
        color VARCHAR(7) NOT NULL,
        description VARCHAR
    );

    CREATE TABLE IF NOT EXISTS node_status (
        device_id UBIGINT PRIMARY KEY,
        address INTEGER,
        battery_level INTEGER,
        error_flags INTEGER,
        signal_strength INTEGER,
        uptime_seconds INTEGER,
        pending_records INTEGER,
        updated_at INTEGER NOT NULL
    );

    CREATE INDEX IF NOT EXISTS idx_node_status_updated
        ON node_status(updated_at);

    CREATE TABLE IF NOT EXISTS node_status_history (
        id INTEGER PRIMARY KEY,
        device_id UBIGINT NOT NULL,
        error_flags INTEGER NOT NULL,
        battery_level INTEGER,
        signal_strength INTEGER,
        uptime_seconds INTEGER,
        pending_records INTEGER,
        timestamp INTEGER NOT NULL
    );

    CREATE INDEX IF NOT EXISTS idx_nsh_device_time
        ON node_status_history(device_id, timestamp);

    CREATE TABLE IF NOT EXISTS node_events (
        id INTEGER PRIMARY KEY,
        device_id UBIGINT NOT NULL,
        timestamp BIGINT NOT NULL,
        event_code INTEGER NOT NULL,
        data_hex VARCHAR,
        received_at INTEGER NOT NULL,
        UNIQUE(device_id, timestamp, event_code, data_hex)
    );

    CREATE INDEX IF NOT EXISTS idx_events_device_time
        ON node_events(device_id, timestamp);

    CREATE TABLE IF NOT EXISTS irrigation_schedules (
        device_id UBIGINT NOT NULL,
        schedule_index INTEGER NOT NULL,
        hour INTEGER NOT NULL,
        minute INTEGER NOT NULL,
        duration INTEGER NOT NULL,
        days INTEGER NOT NULL,
        valve INTEGER NOT NULL,
        created_at INTEGER NOT NULL,
        status VARCHAR DEFAULT 'pending',
        confirmed_at INTEGER,
        PRIMARY KEY (device_id, schedule_index)
    );

    -- Audit trail for ad-hoc dashboard commands (valve run/stop, curtain,
    -- wake-interval). Schedules stay in their own table — this is the
    -- equivalent lifecycle log for fire-and-forget commands so the user
    -- can see "pending" state during the dead zone between click and the
    -- node's confirming event.
    CREATE TABLE IF NOT EXISTS node_commands (
        id BIGINT PRIMARY KEY,
        device_id UBIGINT NOT NULL,
        command_type VARCHAR NOT NULL,
        params VARCHAR NOT NULL,
        status VARCHAR NOT NULL DEFAULT 'pending',
        created_at INTEGER NOT NULL,
        confirmed_at INTEGER,
        expires_at INTEGER NOT NULL,
        huey_task_id VARCHAR,
        confirming_event_code INTEGER,
        confirming_event_detail INTEGER
    );

    CREATE INDEX IF NOT EXISTS idx_cmds_device_created
        ON node_commands(device_id, created_at);

    CREATE INDEX IF NOT EXISTS idx_cmds_pending_match
        ON node_commands(device_id, status, command_type);

    -- Valve groups (master valve). A group links N zone valves to one master
    -- valve on a (usually different) node. The API mirrors each zone window
    -- onto the master node as its own schedule; these tables are the source of
    -- truth, valve_group_master_slots tracking which master slots the API owns.
    CREATE TABLE IF NOT EXISTS valve_groups (
        id INTEGER PRIMARY KEY,
        name VARCHAR NOT NULL,
        master_device_id UBIGINT NOT NULL,
        master_valve INTEGER NOT NULL,
        created_at INTEGER NOT NULL,
        updated_at INTEGER NOT NULL,
        UNIQUE (master_device_id, master_valve)
    );

    CREATE TABLE IF NOT EXISTS valve_group_members (
        group_id INTEGER NOT NULL,
        zone_device_id UBIGINT NOT NULL,
        zone_valve INTEGER NOT NULL,
        PRIMARY KEY (zone_device_id, zone_valve)
    );

    CREATE INDEX IF NOT EXISTS idx_vgm_group
        ON valve_group_members(group_id);

    CREATE TABLE IF NOT EXISTS valve_group_master_slots (
        group_id INTEGER NOT NULL,
        master_device_id UBIGINT NOT NULL,
        master_index INTEGER NOT NULL,
        hour INTEGER NOT NULL,
        minute INTEGER NOT NULL,
        duration INTEGER NOT NULL,
        days INTEGER NOT NULL,
        PRIMARY KEY (master_device_id, master_index)
    );

    CREATE INDEX IF NOT EXISTS idx_vgms_group
        ON valve_group_master_slots(group_id);
    """

    def __init__(self, db_path: str = Config.SENSOR_DB_PATH):
        """Initialize database connection.

        Args:
            db_path: Path to DuckDB database file
        """
        self.db_path = db_path
        self._ensure_directory()
        self._id_counter = 0
        self._id_lock = threading.Lock()
        self._write_buffer: Optional[WriteBuffer] = None
        self._conn: Optional[duckdb.DuckDBPyConnection] = None
        # Reentrant: _get_connection holds the lock and may call
        # _open_connection, which re-acquires the same lock. A plain
        # Lock would deadlock the read thread after a connection reset.
        self._conn_lock = threading.RLock()

    def _ensure_directory(self):
        """Ensure the database directory exists."""
        db_dir = Path(self.db_path).parent
        db_dir.mkdir(parents=True, exist_ok=True)

    def _next_id(self) -> int:
        """Generate next ID for sensor_readings table."""
        with self._id_lock:
            self._id_counter += 1
            return self._id_counter

    def _init_id_counter(self, conn: duckdb.DuckDBPyConnection):
        """Initialize ID counter from existing data.

        The counter is shared across sensor_readings, node_events, and
        node_commands, so seed from the MAX(id) of all three to avoid
        primary-key collisions.
        """
        result = conn.execute("""
            SELECT GREATEST(
                COALESCE((SELECT MAX(id) FROM sensor_readings), 0),
                COALESCE((SELECT MAX(id) FROM node_events), 0),
                COALESCE((SELECT MAX(id) FROM node_commands), 0)
            )
        """).fetchone()
        if result and result[0]:
            self._id_counter = result[0]

    def _open_connection(self) -> duckdb.DuckDBPyConnection:
        """Open (or reopen) the persistent database connection."""
        with self._conn_lock:
            if self._conn is not None:
                try:
                    self._conn.close()
                except Exception:
                    pass
            self._conn = duckdb.connect(self.db_path, config={
                'memory_limit': Config.DB_MEMORY_LIMIT,
                'threads': Config.DB_THREADS,
            })
            return self._conn

    @contextmanager
    def _get_connection(self):
        """Get a cursor from the persistent connection.

        Cursors are lightweight and allow concurrent reads from the
        same underlying connection, sharing the single memory pool.
        """
        with self._conn_lock:
            if self._conn is None:
                self._open_connection()
            conn = self._conn

        cursor = conn.cursor()
        try:
            yield cursor
        except duckdb.Error:
            # Reset connection so the next call reopens it
            with self._conn_lock:
                self._conn = None
            raise
        finally:
            cursor.close()

    def init_db(self):
        """Initialize database schema and open the persistent connection."""
        self._open_connection()
        with self._get_connection() as conn:
            # Strip `-- ...` line comments before splitting so a `;` inside
            # a comment doesn't break statement boundaries (it has). Block
            # comments (/* ... */) are not used in this schema.
            cleaned = '\n'.join(
                line for line in self.SCHEMA.splitlines()
                if not line.strip().startswith('--')
            )
            for statement in cleaned.split(';'):
                statement = statement.strip()
                if statement:
                    conn.execute(statement)
            self._migrate_schema(conn)
            self._init_id_counter(conn)
            logger.info(f"Database initialized at {self.db_path}")

    def _migrate_schema(self, conn):
        """Apply schema migrations for existing databases."""
        # Add status and confirmed_at columns to irrigation_schedules if missing
        try:
            cols = conn.execute(
                "SELECT column_name FROM information_schema.columns "
                "WHERE table_name = 'irrigation_schedules'"
            ).fetchall()
            col_names = {row[0] for row in cols}
            if 'status' not in col_names:
                conn.execute(
                    "ALTER TABLE irrigation_schedules "
                    "ADD COLUMN status VARCHAR DEFAULT 'pending'"
                )
                logger.info("Migrated irrigation_schedules: added status column")
            if 'confirmed_at' not in col_names:
                conn.execute(
                    "ALTER TABLE irrigation_schedules "
                    "ADD COLUMN confirmed_at INTEGER"
                )
                logger.info("Migrated irrigation_schedules: added confirmed_at column")
        except Exception as e:
            logger.warning(f"Schema migration check failed: {e}")

        # Add valve_count column to nodes if missing
        try:
            node_cols = conn.execute(
                "SELECT column_name FROM information_schema.columns "
                "WHERE table_name = 'nodes'"
            ).fetchall()
            if 'valve_count' not in {row[0] for row in node_cols}:
                conn.execute("ALTER TABLE nodes ADD COLUMN valve_count INTEGER")
                logger.info("Migrated nodes: added valve_count column")
        except Exception as e:
            logger.warning(f"Nodes schema migration check failed: {e}")

        # Widen node_events' UNIQUE constraint to include data_hex — the old
        # constraint (device_id, timestamp, event_code) silently dropped
        # distinct same-second, same-type events (e.g. VALVE_OPEN on two
        # different valves) as "duplicates".
        try:
            constraint_rows = conn.execute(
                "SELECT constraint_column_names FROM duckdb_constraints() "
                "WHERE table_name = 'node_events' AND constraint_type = 'UNIQUE'"
            ).fetchall()
            unique_cols = constraint_rows[0][0] if constraint_rows else []
            if 'data_hex' not in unique_cols:
                conn.execute("""
                    CREATE TABLE node_events_new (
                        id INTEGER PRIMARY KEY,
                        device_id UBIGINT NOT NULL,
                        timestamp INTEGER NOT NULL,
                        event_code INTEGER NOT NULL,
                        data_hex VARCHAR,
                        received_at INTEGER NOT NULL,
                        UNIQUE(device_id, timestamp, event_code, data_hex)
                    )
                """)
                conn.execute("INSERT INTO node_events_new SELECT * FROM node_events")
                conn.execute("DROP TABLE node_events")
                conn.execute("ALTER TABLE node_events_new RENAME TO node_events")
                conn.execute(
                    "CREATE INDEX IF NOT EXISTS idx_events_device_time "
                    "ON node_events(device_id, timestamp)"
                )
                logger.info("Migrated node_events: widened UNIQUE constraint to include data_hex")
        except Exception as e:
            logger.warning(f"node_events schema migration check failed: {e}")

        # Widen node_events.timestamp from INTEGER (unix seconds) to BIGINT
        # (unix milliseconds) — firmware/hub now send millisecond-precision
        # event timestamps. Existing rows are second-precision, so multiply
        # by 1000 to keep them on the same millisecond scale as new rows.
        try:
            ts_cols = conn.execute(
                "SELECT data_type FROM information_schema.columns "
                "WHERE table_name = 'node_events' AND column_name = 'timestamp'"
            ).fetchall()
            if ts_cols and ts_cols[0][0] != 'BIGINT':
                conn.execute("""
                    CREATE TABLE node_events_new (
                        id INTEGER PRIMARY KEY,
                        device_id UBIGINT NOT NULL,
                        timestamp BIGINT NOT NULL,
                        event_code INTEGER NOT NULL,
                        data_hex VARCHAR,
                        received_at INTEGER NOT NULL,
                        UNIQUE(device_id, timestamp, event_code, data_hex)
                    )
                """)
                conn.execute("""
                    INSERT INTO node_events_new
                    SELECT id, device_id, timestamp * 1000, event_code, data_hex, received_at
                    FROM node_events
                """)
                conn.execute("DROP TABLE node_events")
                conn.execute("ALTER TABLE node_events_new RENAME TO node_events")
                conn.execute(
                    "CREATE INDEX IF NOT EXISTS idx_events_device_time "
                    "ON node_events(device_id, timestamp)"
                )
                logger.info(
                    "Migrated node_events: timestamp widened to BIGINT milliseconds "
                    "(existing rows scaled x1000)"
                )
        except Exception as e:
            logger.warning(f"node_events timestamp migration check failed: {e}")

    def close(self):
        """Close the persistent database connection."""
        with self._conn_lock:
            if self._conn is not None:
                try:
                    self._conn.close()
                except Exception:
                    pass
                self._conn = None

    def get_write_buffer(self) -> WriteBuffer:
        """Get or create the write buffer for batched inserts."""
        if self._write_buffer is None:
            self._write_buffer = WriteBuffer(self)
        return self._write_buffer

    def insert_reading(self, reading: SensorReading) -> bool:
        """Insert a single sensor reading.

        Args:
            reading: SensorReading to insert

        Returns:
            True if inserted, False if duplicate
        """
        received_at = reading.received_at or int(time.time())
        new_id = self._next_id()

        try:
            with self._get_connection() as conn:
                result = conn.execute("""
                    INSERT INTO sensor_readings
                    (id, device_id, address, timestamp, temperature_centidegrees,
                     humidity_centipercent, flags, received_at)
                    VALUES (?, ?, ?, ?, ?, ?, ?, ?)
                    ON CONFLICT (device_id, timestamp) DO NOTHING
                """, (
                    new_id,
                    reading.device_id,
                    reading.address,
                    reading.timestamp,
                    reading.temperature_centidegrees,
                    reading.humidity_centipercent,
                    reading.flags,
                    received_at
                ))

                # Check if row was inserted
                count = conn.execute(
                    "SELECT COUNT(*) FROM sensor_readings WHERE id = ?",
                    (new_id,)
                ).fetchone()[0]

                if count > 0:
                    # Update node statistics
                    self._update_node_stats(conn, reading.device_id, reading.address, reading.timestamp)
                    return True
                return False

        except duckdb.Error as e:
            logger.error(f"Failed to insert reading: {e}")
            return False

    def insert_batch(self, readings: list[SensorReading]) -> tuple[int, int]:
        """Insert multiple sensor readings in a batch.

        Args:
            readings: List of SensorReading objects

        Returns:
            Tuple of (inserted_count, duplicate_count)
        """
        if not readings:
            return (0, 0)

        received_at = int(time.time())
        inserted = 0
        duplicates = 0

        try:
            with self._get_connection() as conn:
                for reading in readings:
                    ts = reading.received_at or received_at
                    new_id = self._next_id()

                    conn.execute("""
                        INSERT INTO sensor_readings
                        (id, device_id, address, timestamp, temperature_centidegrees,
                         humidity_centipercent, flags, received_at)
                        VALUES (?, ?, ?, ?, ?, ?, ?, ?)
                        ON CONFLICT (device_id, timestamp) DO NOTHING
                    """, (
                        new_id,
                        reading.device_id,
                        reading.address,
                        reading.timestamp,
                        reading.temperature_centidegrees,
                        reading.humidity_centipercent,
                        reading.flags,
                        ts
                    ))

                    # Check if row was inserted
                    count = conn.execute(
                        "SELECT COUNT(*) FROM sensor_readings WHERE id = ?",
                        (new_id,)
                    ).fetchone()[0]

                    if count > 0:
                        inserted += 1
                        self._update_node_stats(conn, reading.device_id, reading.address, reading.timestamp)
                    else:
                        duplicates += 1

                logger.info(f"Batch insert: {inserted} inserted, {duplicates} duplicates")
                return (inserted, duplicates)

        except duckdb.Error as e:
            logger.error(f"Batch insert failed: {e}")
            return (0, len(readings))

    def _update_node_stats(self, conn: duckdb.DuckDBPyConnection, device_id: int, address: Optional[int], timestamp: int):
        """Update node statistics after inserting a reading."""
        # Check if node exists
        existing = conn.execute(
            "SELECT device_id, address, first_seen_at, total_readings FROM nodes WHERE device_id = ?",
            (device_id,)
        ).fetchone()

        if existing:
            # Update existing node, also update address if we have one and it changed
            if address and existing[1] != address:
                conn.execute("""
                    UPDATE nodes SET
                        address = ?,
                        last_seen_at = GREATEST(last_seen_at, ?),
                        total_readings = total_readings + 1
                    WHERE device_id = ?
                """, (address, timestamp, device_id))
            else:
                conn.execute("""
                    UPDATE nodes SET
                        last_seen_at = GREATEST(last_seen_at, ?),
                        total_readings = total_readings + 1
                    WHERE device_id = ?
                """, (timestamp, device_id))
        else:
            conn.execute("""
                INSERT INTO nodes (device_id, address, node_type, first_seen_at, last_seen_at, total_readings)
                VALUES (?, ?, 'SENSOR', ?, ?, 1)
            """, (device_id, address, timestamp, timestamp))

    def query_readings(
        self,
        device_id: Optional[int] = None,
        start_time: Optional[int] = None,
        end_time: Optional[int] = None,
        limit: int = 1000,
        offset: int = 0
    ) -> list[SensorReading]:
        """Query sensor readings with filters.

        Args:
            device_id: Filter by device ID (optional)
            start_time: Start timestamp (optional)
            end_time: End timestamp (optional)
            limit: Maximum records to return
            offset: Number of records to skip

        Returns:
            List of SensorReading objects
        """
        conditions = []
        params = []

        if device_id is not None:
            conditions.append("device_id = ?")
            params.append(device_id)
        if start_time is not None:
            conditions.append("timestamp >= ?")
            params.append(start_time)
        if end_time is not None:
            conditions.append("timestamp <= ?")
            params.append(end_time)

        where_clause = " AND ".join(conditions) if conditions else "1=1"
        params.extend([limit, offset])

        with self._get_connection() as conn:
            result = conn.execute(f"""
                SELECT device_id, address, timestamp, temperature_centidegrees,
                       humidity_centipercent, flags, received_at
                FROM sensor_readings
                WHERE {where_clause}
                ORDER BY timestamp DESC
                LIMIT ? OFFSET ?
            """, params)

            return [
                SensorReading(
                    device_id=row[0],
                    address=row[1],
                    timestamp=row[2],
                    temperature_centidegrees=row[3],
                    humidity_centipercent=row[4],
                    flags=row[5],
                    received_at=row[6]
                )
                for row in result.fetchall()
            ]

    def get_latest_reading(self, device_id: int) -> Optional[SensorReading]:
        """Get the most recent reading for a node.

        Args:
            device_id: Device ID to query

        Returns:
            Latest SensorReading or None
        """
        readings = self.query_readings(device_id=device_id, limit=1)
        return readings[0] if readings else None

    def query_readings_downsampled(
        self,
        device_id: int,
        start_time: int,
        end_time: int,
        max_points: int = 500
    ) -> list[dict]:
        """Query sensor readings with bucket averaging for chart display.

        Divides the time range into buckets and returns averaged values
        for each bucket. This dramatically reduces payload size while
        preserving the overall trend.

        Args:
            device_id: Device ID to query
            start_time: Start timestamp (required)
            end_time: End timestamp (required)
            max_points: Maximum number of data points to return

        Returns:
            List of dicts with timestamp, temperature_celsius, humidity_percent
        """
        time_range = end_time - start_time
        if time_range <= 0:
            return []

        # Calculate bucket size in seconds
        bucket_size = max(1, time_range // max_points)

        with self._get_connection() as conn:
            result = conn.execute("""
                SELECT
                    (timestamp / ?) * ? as bucket_timestamp,
                    AVG(temperature_centidegrees) as avg_temp,
                    AVG(humidity_centipercent) as avg_hum,
                    COUNT(*) as sample_count,
                    BIT_OR(flags) as flags
                FROM sensor_readings
                WHERE device_id = ?
                  AND timestamp >= ?
                  AND timestamp <= ?
                GROUP BY bucket_timestamp
                ORDER BY bucket_timestamp ASC
            """, (bucket_size, bucket_size, device_id, start_time, end_time))

            return [
                {
                    'timestamp': int(row[0]) + bucket_size // 2,  # Use bucket midpoint
                    'temperature_celsius': round(row[1] / 100.0, 2) if row[1] else None,
                    'humidity_percent': round(row[2] / 100.0, 2) if row[2] else None,
                    'sample_count': row[3],
                    **(({'flags': row[4]}) if row[4] else {}),
                }
                for row in result.fetchall()
            ]

    def get_node_statistics(
        self,
        device_id: int,
        start_time: Optional[int] = None,
        end_time: Optional[int] = None
    ) -> Optional[dict]:
        """Get statistics for a specific node.

        Args:
            device_id: Device ID to query
            start_time: Start timestamp for stats calculation (optional)
            end_time: End timestamp for stats calculation (optional)

        Returns:
            Dictionary with node statistics or None
        """
        with self._get_connection() as conn:
            # Get node info
            result = conn.execute("""
                SELECT device_id, address, node_type, first_seen_at, last_seen_at, total_readings
                FROM nodes WHERE device_id = ?
            """, (device_id,))
            row = result.fetchone()

            if not row:
                return None

            # Build time-filtered stats query
            conditions = ["device_id = ?"]
            params = [device_id]

            if start_time is not None:
                conditions.append("timestamp >= ?")
                params.append(start_time)
            if end_time is not None:
                conditions.append("timestamp <= ?")
                params.append(end_time)

            where_clause = " AND ".join(conditions)

            # Get temperature stats (filtered by time range if provided)
            stats_result = conn.execute(f"""
                SELECT
                    MIN(temperature_centidegrees) as min_temp,
                    MAX(temperature_centidegrees) as max_temp,
                    AVG(temperature_centidegrees) as avg_temp,
                    MIN(humidity_centipercent) as min_hum,
                    MAX(humidity_centipercent) as max_hum,
                    AVG(humidity_centipercent) as avg_hum,
                    COUNT(*) as reading_count
                FROM sensor_readings
                WHERE {where_clause}
            """, params)
            stats = stats_result.fetchone()

            # Use filtered reading count if time range provided, otherwise total
            reading_count = stats[6] if (start_time or end_time) else row[5]

            return {
                'device_id': str(row[0]),  # String to preserve JS precision
                'address': row[1],
                'node_type': row[2],
                'first_seen_at': row[3],
                'last_seen_at': row[4],
                'total_readings': row[5],
                'reading_count': reading_count,
                'temperature': {
                    'min_celsius': stats[0] / 100.0 if stats[0] else None,
                    'max_celsius': stats[1] / 100.0 if stats[1] else None,
                    'avg_celsius': stats[2] / 100.0 if stats[2] else None,
                },
                'humidity': {
                    'min_percent': stats[3] / 100.0 if stats[3] else None,
                    'max_percent': stats[4] / 100.0 if stats[4] else None,
                    'avg_percent': stats[5] / 100.0 if stats[5] else None,
                }
            }

    def get_reading_count(
        self,
        device_id: Optional[int] = None,
        start_time: Optional[int] = None,
        end_time: Optional[int] = None
    ) -> int:
        """Get count of readings matching filters.

        Args:
            device_id: Filter by device ID (optional)
            start_time: Start timestamp (optional)
            end_time: End timestamp (optional)

        Returns:
            Count of matching readings
        """
        conditions = []
        params = []

        if device_id is not None:
            conditions.append("device_id = ?")
            params.append(device_id)
        if start_time is not None:
            conditions.append("timestamp >= ?")
            params.append(start_time)
        if end_time is not None:
            conditions.append("timestamp <= ?")
            params.append(end_time)

        where_clause = " AND ".join(conditions) if conditions else "1=1"

        with self._get_connection() as conn:
            result = conn.execute(f"""
                SELECT COUNT(*) as count FROM sensor_readings WHERE {where_clause}
            """, params)
            return result.fetchone()[0]

    def export_csv_iter(
        self,
        device_id: Optional[int] = None,
        start_time: Optional[int] = None,
        end_time: Optional[int] = None
    ):
        """Export readings as a CSV line generator (streaming).

        Yields one row at a time instead of building the full result
        in memory, keeping memory usage constant regardless of data size.

        Args:
            device_id: Filter by device ID (optional)
            start_time: Start timestamp (optional)
            end_time: End timestamp (optional)

        Yields:
            CSV lines including header
        """
        yield "device_id,address,timestamp,temperature_celsius,humidity_percent,flags\n"

        conditions = []
        params = []

        if device_id is not None:
            conditions.append("device_id = ?")
            params.append(device_id)
        if start_time is not None:
            conditions.append("timestamp >= ?")
            params.append(start_time)
        if end_time is not None:
            conditions.append("timestamp <= ?")
            params.append(end_time)

        where_clause = " AND ".join(conditions) if conditions else "1=1"

        with self._get_connection() as conn:
            result = conn.execute(f"""
                SELECT device_id, address, timestamp, temperature_centidegrees,
                       humidity_centipercent, flags
                FROM sensor_readings
                WHERE {where_clause}
                ORDER BY timestamp DESC
            """, params)

            while True:
                row = result.fetchone()
                if row is None:
                    break
                temp_c = row[3] / 100.0
                hum_p = row[4] / 100.0
                yield f"{row[0]},{row[1] or ''},{row[2]},{temp_c:.2f},{hum_p:.2f},{row[5]}\n"

    def get_node_metadata(self, device_id: int) -> Optional[dict]:
        """Get metadata for a node.

        Args:
            device_id: Device ID

        Returns:
            Dictionary with node metadata or None if not found
        """
        with self._get_connection() as conn:
            result = conn.execute("""
                SELECT device_id, name, notes, zone_id, updated_at
                FROM node_metadata WHERE device_id = ?
            """, (device_id,))
            row = result.fetchone()

            if not row:
                return None

            return {
                'device_id': str(row[0]),  # String to preserve JS precision
                'name': row[1],
                'notes': row[2],
                'zone_id': row[3],
                'updated_at': row[4]
            }

    def get_all_node_metadata(self) -> dict[int, dict]:
        """Get metadata for all nodes.

        Returns:
            Dictionary mapping device_id to metadata dict
        """
        with self._get_connection() as conn:
            result = conn.execute("""
                SELECT device_id, name, notes, zone_id, updated_at
                FROM node_metadata
            """)

            metadata = {}
            for row in result.fetchall():
                metadata[row[0]] = {
                    'device_id': str(row[0]),  # String to preserve JS precision
                    'name': row[1],
                    'notes': row[2],
                    'zone_id': row[3],
                    'updated_at': row[4]
                }
            return metadata

    def update_node_metadata(
        self,
        device_id: int,
        name: Optional[str] = None,
        notes: Optional[str] = None,
        zone_id: Optional[int] = None
    ) -> dict:
        """Update metadata for a node (upsert).

        Args:
            device_id: Device ID
            name: Friendly name for the node
            notes: Additional notes
            zone_id: Zone ID (use -1 to explicitly unset, None to preserve)

        Returns:
            Updated metadata dictionary
        """
        updated_at = int(time.time())

        with self._get_connection() as conn:
            # Check if metadata exists
            existing = conn.execute(
                "SELECT name, notes, zone_id FROM node_metadata WHERE device_id = ?",
                (device_id,)
            ).fetchone()

            if existing:
                # Update existing - only update fields that are provided
                current_name = name if name is not None else existing[0]
                current_notes = notes if notes is not None else existing[1]
                # zone_id: -1 means explicitly unset, None means preserve current
                if zone_id == -1:
                    current_zone_id = None
                elif zone_id is not None:
                    current_zone_id = zone_id
                else:
                    current_zone_id = existing[2]

                conn.execute("""
                    UPDATE node_metadata
                    SET name = ?, notes = ?, zone_id = ?, updated_at = ?
                    WHERE device_id = ?
                """, (current_name, current_notes, current_zone_id, updated_at, device_id))
            else:
                # Insert new
                actual_zone_id = None if zone_id == -1 else zone_id
                conn.execute("""
                    INSERT INTO node_metadata (device_id, name, notes, zone_id, updated_at)
                    VALUES (?, ?, ?, ?, ?)
                """, (device_id, name, notes, actual_zone_id, updated_at))

        return self.get_node_metadata(device_id)

    def sync_to_s3(
        self,
        bucket: str = Config.S3_BUCKET,
        prefix: str = Config.S3_PREFIX,
        region: str = Config.S3_REGION
    ) -> bool:
        """Export sensor data to S3 as Parquet. Returns True on success.

        Args:
            bucket: S3 bucket name (empty to disable)
            prefix: Key prefix within bucket
            region: AWS region

        Returns:
            True if sync succeeded, False otherwise
        """
        if not bucket:
            logger.debug("S3 sync disabled (no bucket configured)")
            return False

        try:
            with self._get_connection() as conn:
                # Install and load httpfs extension
                conn.execute("INSTALL httpfs; LOAD httpfs;")
                conn.execute(f"SET s3_region='{region}';")

                # Export sensor readings to Parquet
                s3_path = f"s3://{bucket}/{prefix}sensor_readings.parquet"
                conn.execute(f"COPY sensor_readings TO '{s3_path}' (FORMAT PARQUET)")

                logger.info(f"Synced database to {s3_path}")
                return True

        except Exception as e:
            logger.warning(f"S3 sync failed (non-fatal): {e}")
            return False

    # ===== Zone Management =====

    def create_zone(self, name: str, color: str, description: Optional[str] = None) -> dict:
        """Create a new zone.

        Args:
            name: Zone name
            color: Hex color code (e.g., '#4CAF50')
            description: Optional description

        Returns:
            Created zone dictionary
        """
        with self._get_connection() as conn:
            # Get next ID
            result = conn.execute("SELECT COALESCE(MAX(id), 0) + 1 FROM zones")
            next_id = result.fetchone()[0]

            conn.execute("""
                INSERT INTO zones (id, name, color, description)
                VALUES (?, ?, ?, ?)
            """, (next_id, name, color, description))

            return self.get_zone(next_id)

    def get_zone(self, zone_id: int) -> Optional[dict]:
        """Get a zone by ID.

        Args:
            zone_id: Zone ID

        Returns:
            Zone dictionary or None if not found
        """
        with self._get_connection() as conn:
            result = conn.execute("""
                SELECT id, name, color, description
                FROM zones WHERE id = ?
            """, (zone_id,))
            row = result.fetchone()

            if not row:
                return None

            return {
                'id': row[0],
                'name': row[1],
                'color': row[2],
                'description': row[3]
            }

    def get_all_zones(self) -> list[dict]:
        """Get all zones.

        Returns:
            List of zone dictionaries
        """
        with self._get_connection() as conn:
            result = conn.execute("""
                SELECT id, name, color, description
                FROM zones ORDER BY name
            """)

            return [
                {
                    'id': row[0],
                    'name': row[1],
                    'color': row[2],
                    'description': row[3]
                }
                for row in result.fetchall()
            ]

    def update_zone(
        self,
        zone_id: int,
        name: Optional[str] = None,
        color: Optional[str] = None,
        description: Optional[str] = None
    ) -> Optional[dict]:
        """Update a zone.

        Args:
            zone_id: Zone ID
            name: New name (optional)
            color: New color (optional)
            description: New description (optional, use empty string to clear)

        Returns:
            Updated zone dictionary or None if not found
        """
        with self._get_connection() as conn:
            existing = conn.execute(
                "SELECT name, color, description FROM zones WHERE id = ?",
                (zone_id,)
            ).fetchone()

            if not existing:
                return None

            current_name = name if name is not None else existing[0]
            current_color = color if color is not None else existing[1]
            current_description = description if description is not None else existing[2]

            conn.execute("""
                UPDATE zones
                SET name = ?, color = ?, description = ?
                WHERE id = ?
            """, (current_name, current_color, current_description, zone_id))

        return self.get_zone(zone_id)

    def delete_zone(self, zone_id: int) -> bool:
        """Delete a zone. Nodes in this zone become unzoned.

        Args:
            zone_id: Zone ID

        Returns:
            True if deleted, False if not found
        """
        with self._get_connection() as conn:
            # Check if zone exists
            existing = conn.execute(
                "SELECT id FROM zones WHERE id = ?",
                (zone_id,)
            ).fetchone()

            if not existing:
                return False

            # Unset zone_id for all nodes in this zone
            conn.execute("""
                UPDATE node_metadata
                SET zone_id = NULL
                WHERE zone_id = ?
            """, (zone_id,))

            # Delete the zone
            conn.execute("DELETE FROM zones WHERE id = ?", (zone_id,))

            return True

    # --- Valve groups (master valve) ---

    def _valve_group_row(self, conn, group_id: int) -> Optional[dict]:
        """Build a valve-group dict (with members) from an open cursor."""
        row = conn.execute("""
            SELECT id, name, master_device_id, master_valve, created_at, updated_at
            FROM valve_groups WHERE id = ?
        """, (group_id,)).fetchone()
        if not row:
            return None
        members = conn.execute("""
            SELECT zone_device_id, zone_valve FROM valve_group_members
            WHERE group_id = ? ORDER BY zone_device_id, zone_valve
        """, (group_id,)).fetchall()
        return {
            'id': row[0],
            'name': row[1],
            'master_device_id': str(row[2]),  # String to preserve JS precision
            'master_valve': row[3],
            'created_at': row[4],
            'updated_at': row[5],
            'members': [
                {'zone_device_id': str(m[0]), 'zone_valve': m[1]} for m in members
            ],
        }

    def create_valve_group(self, name: str, master_device_id: int, master_valve: int,
                           members: list[dict]) -> dict:
        """Create a valve group with its zone members.

        Args:
            name: Human-readable group name
            master_device_id: Device ID of the node holding the master valve
            master_valve: Valve index of the master valve on that node
            members: List of {'zone_device_id': int, 'zone_valve': int}

        Returns:
            Created group dict. Raises duckdb.Error on constraint violation
            (master already in use, or a zone valve already in another group).
        """
        now = int(time.time())
        with self._get_connection() as conn:
            next_id = conn.execute(
                "SELECT COALESCE(MAX(id), 0) + 1 FROM valve_groups"
            ).fetchone()[0]
            conn.execute("""
                INSERT INTO valve_groups
                    (id, name, master_device_id, master_valve, created_at, updated_at)
                VALUES (?, ?, ?, ?, ?, ?)
            """, (next_id, name, master_device_id, master_valve, now, now))
            for member in members:
                conn.execute("""
                    INSERT INTO valve_group_members (group_id, zone_device_id, zone_valve)
                    VALUES (?, ?, ?)
                """, (next_id, int(member['zone_device_id']), int(member['zone_valve'])))
            return self._valve_group_row(conn, next_id)

    def get_valve_group(self, group_id: int) -> Optional[dict]:
        """Get a valve group (with members) by ID, or None."""
        with self._get_connection() as conn:
            return self._valve_group_row(conn, group_id)

    def get_all_valve_groups(self) -> list[dict]:
        """Get all valve groups (with members)."""
        with self._get_connection() as conn:
            ids = conn.execute("SELECT id FROM valve_groups ORDER BY name").fetchall()
            return [self._valve_group_row(conn, row[0]) for row in ids]

    def update_valve_group(self, group_id: int, name: Optional[str] = None,
                           master_device_id: Optional[int] = None,
                           master_valve: Optional[int] = None,
                           members: Optional[list[dict]] = None) -> Optional[dict]:
        """Update a valve group. Pass members to replace the membership wholesale.

        Returns the updated group dict, or None if it does not exist.
        """
        now = int(time.time())
        with self._get_connection() as conn:
            existing = conn.execute(
                "SELECT name, master_device_id, master_valve FROM valve_groups WHERE id = ?",
                (group_id,)
            ).fetchone()
            if not existing:
                return None
            new_name = name if name is not None else existing[0]
            new_master_device = master_device_id if master_device_id is not None else existing[1]
            new_master_valve = master_valve if master_valve is not None else existing[2]
            conn.execute("""
                UPDATE valve_groups
                SET name = ?, master_device_id = ?, master_valve = ?, updated_at = ?
                WHERE id = ?
            """, (new_name, new_master_device, new_master_valve, now, group_id))
            if members is not None:
                conn.execute("DELETE FROM valve_group_members WHERE group_id = ?", (group_id,))
                for member in members:
                    conn.execute("""
                        INSERT INTO valve_group_members (group_id, zone_device_id, zone_valve)
                        VALUES (?, ?, ?)
                    """, (group_id, int(member['zone_device_id']), int(member['zone_valve'])))
            return self._valve_group_row(conn, group_id)

    def delete_valve_group(self, group_id: int) -> bool:
        """Delete a valve group and its members + owned master slots.

        Returns True if the group existed. The caller is responsible for
        queueing REMOVE_SCHEDULE for the master slots before calling this.
        """
        with self._get_connection() as conn:
            existing = conn.execute(
                "SELECT id FROM valve_groups WHERE id = ?", (group_id,)
            ).fetchone()
            if not existing:
                return False
            conn.execute("DELETE FROM valve_group_members WHERE group_id = ?", (group_id,))
            conn.execute("DELETE FROM valve_group_master_slots WHERE group_id = ?", (group_id,))
            conn.execute("DELETE FROM valve_groups WHERE id = ?", (group_id,))
            return True

    def get_group_for_zone_valve(self, zone_device_id: int,
                                 zone_valve: int) -> Optional[dict]:
        """Return the group (with members) a given zone valve belongs to, or None."""
        with self._get_connection() as conn:
            row = conn.execute("""
                SELECT group_id FROM valve_group_members
                WHERE zone_device_id = ? AND zone_valve = ?
            """, (zone_device_id, zone_valve)).fetchone()
            if not row:
                return None
            return self._valve_group_row(conn, row[0])

    def list_master_slots(self, master_device_id: int) -> list[dict]:
        """List the master schedule slots the API currently owns for a master node."""
        with self._get_connection() as conn:
            rows = conn.execute("""
                SELECT group_id, master_index, hour, minute, duration, days
                FROM valve_group_master_slots
                WHERE master_device_id = ?
                ORDER BY master_index
            """, (master_device_id,)).fetchall()
            return [{
                'group_id': r[0],
                'master_index': r[1],
                'hour': r[2],
                'minute': r[3],
                'duration': r[4],
                'days': r[5],
            } for r in rows]

    def replace_master_slots(self, group_id: int, master_device_id: int,
                             slots: list[dict]) -> None:
        """Replace the stored master slots for a group with the given set.

        Each slot is {'master_index', 'hour', 'minute', 'duration', 'days'}.
        Call AFTER computing the SET/REMOVE diff against the previous slots.
        """
        with self._get_connection() as conn:
            conn.execute(
                "DELETE FROM valve_group_master_slots WHERE group_id = ?", (group_id,))
            for slot in slots:
                conn.execute("""
                    INSERT INTO valve_group_master_slots
                        (group_id, master_device_id, master_index, hour, minute, duration, days)
                    VALUES (?, ?, ?, ?, ?, ?, ?)
                """, (group_id, master_device_id, slot['master_index'], slot['hour'],
                      slot['minute'], slot['duration'], slot['days']))

    def set_node_zone(self, device_id: int, zone_id: Optional[int]) -> Optional[dict]:
        """Set a node's zone.

        Args:
            device_id: Device ID
            zone_id: Zone ID or None to unzone

        Returns:
            Updated node metadata or None if node not found
        """
        # Use -1 to explicitly unset zone_id in update_node_metadata
        zone_value = -1 if zone_id is None else zone_id
        return self.update_node_metadata(device_id, zone_id=zone_value)

    # ===== Node Status Management =====

    def update_node_status(
        self,
        device_id: int,
        address: Optional[int] = None,
        battery_level: Optional[int] = None,
        error_flags: Optional[int] = None,
        signal_strength: Optional[int] = None,
        uptime_seconds: Optional[int] = None,
        pending_records: Optional[int] = None
    ) -> dict:
        """Update status for a node (upsert).

        Also records history when error_flags change, enabling analysis
        of when errors occurred over time.

        Args:
            device_id: Hardware device ID (primary key)
            address: LoRa address (for routing)
            battery_level: Battery percentage (0-100, 255=external power)
            error_flags: Bitmask of error flags
            signal_strength: RSSI in dBm (negative value)
            uptime_seconds: Node uptime in seconds
            pending_records: Untransmitted sensor records in flash backlog

        Returns:
            Updated status dictionary
        """
        updated_at = int(time.time())

        with self._get_connection() as conn:
            # Check if status exists
            existing = conn.execute(
                "SELECT address, battery_level, error_flags, signal_strength, uptime_seconds, pending_records FROM node_status WHERE device_id = ?",
                (device_id,)
            ).fetchone()

            # Track previous error_flags for history
            previous_error_flags = existing[2] if existing else None

            if existing:
                # Update existing - only update fields that are provided
                current_address = address if address is not None else existing[0]
                current_battery = battery_level if battery_level is not None else existing[1]
                current_errors = error_flags if error_flags is not None else existing[2]
                current_signal = signal_strength if signal_strength is not None else existing[3]
                current_uptime = uptime_seconds if uptime_seconds is not None else existing[4]
                current_pending = pending_records if pending_records is not None else existing[5]

                conn.execute("""
                    UPDATE node_status
                    SET address = ?, battery_level = ?, error_flags = ?,
                        signal_strength = ?, uptime_seconds = ?, pending_records = ?, updated_at = ?
                    WHERE device_id = ?
                """, (current_address, current_battery, current_errors,
                      current_signal, current_uptime, current_pending, updated_at, device_id))
            else:
                # Insert new
                conn.execute("""
                    INSERT INTO node_status (device_id, address, battery_level, error_flags,
                                            signal_strength, uptime_seconds, pending_records, updated_at)
                    VALUES (?, ?, ?, ?, ?, ?, ?, ?)
                """, (device_id, address, battery_level, error_flags,
                      signal_strength, uptime_seconds, pending_records, updated_at))

            # Record history if error_flags changed (or first status report)
            if error_flags is not None and error_flags != previous_error_flags:
                self._record_status_history(
                    conn, device_id, error_flags, battery_level, signal_strength, uptime_seconds, pending_records, updated_at
                )

        return self.get_node_status(device_id)

    def _record_status_history(
        self,
        conn,
        device_id: int,
        error_flags: int,
        battery_level: Optional[int],
        signal_strength: Optional[int],
        uptime_seconds: Optional[int],
        pending_records: Optional[int],
        timestamp: int
    ) -> None:
        """Record a status history entry (internal helper)."""
        # Get next history ID
        result = conn.execute("SELECT COALESCE(MAX(id), 0) + 1 FROM node_status_history")
        next_id = result.fetchone()[0]

        conn.execute("""
            INSERT INTO node_status_history
            (id, device_id, error_flags, battery_level, signal_strength, uptime_seconds, pending_records, timestamp)
            VALUES (?, ?, ?, ?, ?, ?, ?, ?)
        """, (next_id, device_id, error_flags, battery_level, signal_strength, uptime_seconds, pending_records, timestamp))

    def get_node_status(self, device_id: int) -> Optional[dict]:
        """Get status for a node.

        Args:
            device_id: Device ID

        Returns:
            Status dictionary or None if not found
        """
        with self._get_connection() as conn:
            result = conn.execute("""
                SELECT device_id, address, battery_level, error_flags,
                       signal_strength, uptime_seconds, pending_records, updated_at
                FROM node_status WHERE device_id = ?
            """, (device_id,))
            row = result.fetchone()

            if not row:
                return None

            return {
                'device_id': str(row[0]),  # String to preserve JS precision
                'address': row[1],
                'battery_level': row[2],
                'error_flags': row[3],
                'signal_strength': row[4],
                'uptime_seconds': row[5],
                'pending_records': row[6],
                'updated_at': row[7]
            }

    def get_all_node_status(self) -> dict[int, dict]:
        """Get status for all nodes.

        Returns:
            Dictionary mapping device_id to status dict
        """
        with self._get_connection() as conn:
            result = conn.execute("""
                SELECT device_id, address, battery_level, error_flags,
                       signal_strength, uptime_seconds, pending_records, updated_at
                FROM node_status
            """)

            status = {}
            for row in result.fetchall():
                status[row[0]] = {
                    'device_id': str(row[0]),  # String to preserve JS precision
                    'address': row[1],
                    'battery_level': row[2],
                    'error_flags': row[3],
                    'signal_strength': row[4],
                    'uptime_seconds': row[5],
                    'pending_records': row[6],
                    'updated_at': row[7]
                }
            return status

    def get_node_error_history(
        self,
        device_id: int,
        start_time: int,
        end_time: int
    ) -> list[dict]:
        """Get error flag changes for a node in time range.

        Returns history entries where error_flags changed, useful for
        tracking when errors like flash failures occurred.

        Args:
            device_id: Device ID
            start_time: Start timestamp (inclusive)
            end_time: End timestamp (inclusive)

        Returns:
            List of dicts with error_flags, battery_level, signal_strength,
            uptime_seconds, pending_records, and timestamp
        """
        with self._get_connection() as conn:
            result = conn.execute("""
                SELECT error_flags, battery_level, signal_strength, uptime_seconds, pending_records, timestamp
                FROM node_status_history
                WHERE device_id = ? AND timestamp BETWEEN ? AND ?
                ORDER BY timestamp ASC
            """, (device_id, start_time, end_time))

            return [
                {
                    'error_flags': row[0],
                    'battery_level': row[1],
                    'signal_strength': row[2],
                    'uptime_seconds': row[3],
                    'pending_records': row[4],
                    'timestamp': row[5]
                }
                for row in result.fetchall()
            ]

    def delete_node(self, device_id: int) -> bool:
        """Delete a node and all its associated data.

        Removes data from all related tables:
        - nodes
        - node_metadata
        - node_status
        - node_status_history
        - sensor_readings

        Args:
            device_id: Device ID

        Returns:
            True if node was deleted, False if not found
        """
        with self._get_connection() as conn:
            # Check if node exists in any table
            exists = conn.execute(
                "SELECT 1 FROM nodes WHERE device_id = ? "
                "UNION SELECT 1 FROM node_metadata WHERE device_id = ? "
                "UNION SELECT 1 FROM node_status WHERE device_id = ? "
                "UNION SELECT 1 FROM sensor_readings WHERE device_id = ? "
                "LIMIT 1",
                (device_id, device_id, device_id, device_id)
            ).fetchone()

            if not exists:
                return False

            # Delete from all related tables
            conn.execute("DELETE FROM sensor_readings WHERE device_id = ?", (device_id,))
            conn.execute("DELETE FROM node_status_history WHERE device_id = ?", (device_id,))
            conn.execute("DELETE FROM node_status WHERE device_id = ?", (device_id,))
            conn.execute("DELETE FROM node_metadata WHERE device_id = ?", (device_id,))
            conn.execute("DELETE FROM irrigation_schedules WHERE device_id = ?", (device_id,))
            # Valve groups: drop this node's zone memberships, and any group it
            # was the master of (plus that group's members and owned slots).
            conn.execute("DELETE FROM valve_group_members WHERE zone_device_id = ?", (device_id,))
            conn.execute(
                "DELETE FROM valve_group_members WHERE group_id IN "
                "(SELECT id FROM valve_groups WHERE master_device_id = ?)", (device_id,))
            conn.execute(
                "DELETE FROM valve_group_master_slots WHERE master_device_id = ?", (device_id,))
            conn.execute("DELETE FROM valve_groups WHERE master_device_id = ?", (device_id,))
            conn.execute("DELETE FROM nodes WHERE device_id = ?", (device_id,))

            logger.info(f"Deleted node with device_id {device_id} and all associated data")
            return True

    def get_node_by_device_id(self, device_id: int) -> Optional[dict]:
        """Get node info by device_id.

        Args:
            device_id: Device ID to look up

        Returns:
            Node dictionary with address and other info, or None
        """
        with self._get_connection() as conn:
            result = conn.execute("""
                SELECT device_id, address, node_type, first_seen_at, last_seen_at,
                       total_readings, valve_count
                FROM nodes WHERE device_id = ?
            """, (device_id,))
            row = result.fetchone()

            if not row:
                return None

            return {
                'device_id': str(row[0]),  # String to preserve JS precision
                'address': row[1],
                'node_type': row[2],
                'first_seen_at': row[3],
                'last_seen_at': row[4],
                'total_readings': row[5],
                'valve_count': row[6]
            }

    def set_node_valve_count(self, device_id: int, valve_count: int) -> None:
        """Persist a node's valve count (upsert).

        Called when the hub reports a valve_count for a node, so the DB-fallback
        path can return it when the hub is unreachable. A node reporting a valve
        count is necessarily a valve/irrigation node, so a row is created with that
        type if one does not already exist.

        Args:
            device_id: Hardware device ID (primary key)
            valve_count: Number of valves reported by the node
        """
        now = int(time.time())
        with self._get_connection() as conn:
            existing = conn.execute(
                "SELECT device_id FROM nodes WHERE device_id = ?", (device_id,)
            ).fetchone()
            if existing:
                conn.execute(
                    "UPDATE nodes SET valve_count = ? WHERE device_id = ?",
                    (valve_count, device_id)
                )
            else:
                conn.execute("""
                    INSERT INTO nodes
                    (device_id, address, node_type, first_seen_at, last_seen_at,
                     total_readings, valve_count)
                    VALUES (?, NULL, 'IRRIGATION', ?, ?, 0, ?)
                """, (device_id, now, now, valve_count))

    def insert_event(self, device_id: int, timestamp: int, event_code: int,
                     data_hex: str = "") -> bool:
        """Insert a node event.

        Args:
            device_id: Device identifier
            timestamp: Event timestamp (unix milliseconds)
            event_code: Event type code
            data_hex: Hex-encoded event detail data

        Returns:
            True if inserted, False if duplicate
        """
        received_at = int(time.time())
        new_id = self._next_id()

        try:
            with self._get_connection() as conn:
                conn.execute("""
                    INSERT INTO node_events
                    (id, device_id, timestamp, event_code, data_hex, received_at)
                    VALUES (?, ?, ?, ?, ?, ?)
                    ON CONFLICT (device_id, timestamp, event_code, data_hex) DO NOTHING
                """, (new_id, device_id, timestamp, event_code, data_hex, received_at))

                count = conn.execute(
                    "SELECT COUNT(*) FROM node_events WHERE id = ?",
                    (new_id,)
                ).fetchone()[0]
                return count > 0

        except duckdb.Error as e:
            logger.error(f"Failed to insert event: {e}")
            return False

    def query_events(self, device_id: int, start_time: int = None,
                     end_time: int = None, limit: int = 100) -> list[dict]:
        """Query events for a device.

        Args:
            device_id: Device identifier
            start_time: Start timestamp filter (optional)
            end_time: End timestamp filter (optional)
            limit: Maximum results (default 100)

        Returns:
            List of event dicts
        """
        try:
            with self._get_connection() as conn:
                conditions = ["device_id = ?"]
                params = [device_id]

                if start_time is not None:
                    conditions.append("timestamp >= ?")
                    params.append(start_time)
                if end_time is not None:
                    conditions.append("timestamp <= ?")
                    params.append(end_time)

                where_clause = " AND ".join(conditions)
                params.append(min(limit, 1000))

                result = conn.execute(f"""
                    SELECT device_id, timestamp, event_code, data_hex, received_at
                    FROM node_events
                    WHERE {where_clause}
                    ORDER BY timestamp DESC
                    LIMIT ?
                """, params)

                rows = result.fetchall()
                return [{
                    'device_id': str(row[0]),
                    'timestamp': row[1],
                    'event_code': row[2],
                    'data_hex': row[3] or "",
                    'received_at': row[4],
                } for row in rows]

        except duckdb.Error as e:
            logger.error(f"Failed to query events: {e}")
            return []

    # ------------------------------------------------------------------
    # Node commands — audit trail for ad-hoc dashboard commands.
    # ------------------------------------------------------------------

    def insert_command(self, device_id: int, command_type: str,
                       params: dict, ttl_seconds: int,
                       huey_task_id: Optional[str] = None) -> Optional[int]:
        """Record a dashboard-issued command and return its id.

        Args:
            device_id: Device identifier
            command_type: 'valve_open' | 'valve_close' | 'curtain' | 'wake_interval'
            params: Command-specific parameters, JSON-serialisable
            ttl_seconds: How long to wait for confirmation before marking expired
            huey_task_id: Huey task id once enqueued (may be set later)

        Returns:
            The new command's id, or None on failure.
        """
        new_id = self._next_id()
        now = int(time.time())
        params_json = json.dumps(params, sort_keys=True)
        try:
            with self._get_connection() as conn:
                conn.execute("""
                    INSERT INTO node_commands
                    (id, device_id, command_type, params, status,
                     created_at, expires_at, huey_task_id)
                    VALUES (?, ?, ?, ?, 'pending', ?, ?, ?)
                """, (new_id, device_id, command_type, params_json,
                      now, now + ttl_seconds, huey_task_id))
                return new_id
        except duckdb.Error as e:
            logger.error(f"Failed to insert command: {e}")
            return None

    def set_command_huey_task(self, command_id: int, huey_task_id: str) -> bool:
        """Backfill the huey task id after the command has been enqueued."""
        try:
            with self._get_connection() as conn:
                conn.execute(
                    "UPDATE node_commands SET huey_task_id = ? WHERE id = ?",
                    (huey_task_id, command_id),
                )
            return True
        except duckdb.Error as e:
            logger.error(f"Failed to set huey_task_id on command {command_id}: {e}")
            return False

    def find_pending_command(self, device_id: int, command_type: str,
                             param_filter: Optional[dict] = None) -> Optional[dict]:
        """Find the oldest pending command for matching against a node event.

        Args:
            device_id: Device identifier
            command_type: Command type to match
            param_filter: Optional dict of param key→value pairs that must all
                          match exactly. Used to distinguish e.g. valve 0 vs
                          valve 1 commands, or curtain open vs close.

        Returns:
            Command dict (id, params, created_at, ...) or None.
        """
        try:
            with self._get_connection() as conn:
                result = conn.execute("""
                    SELECT id, device_id, command_type, params,
                           status, created_at, expires_at
                    FROM node_commands
                    WHERE device_id = ?
                      AND command_type = ?
                      AND status = 'pending'
                    ORDER BY created_at ASC
                """, (device_id, command_type))
                for row in result.fetchall():
                    try:
                        row_params = json.loads(row[3])
                    except (ValueError, TypeError):
                        row_params = {}
                    if param_filter:
                        if not all(row_params.get(k) == v
                                   for k, v in param_filter.items()):
                            continue
                    return {
                        'id': row[0],
                        'device_id': str(row[1]),
                        'command_type': row[2],
                        'params': row_params,
                        'status': row[4],
                        'created_at': row[5],
                        'expires_at': row[6],
                    }
            return None
        except duckdb.Error as e:
            logger.error(f"Failed to find pending command: {e}")
            return None

    def update_command_status(self, command_id: int, status: str,
                              timestamp: Optional[int] = None,
                              event_code: Optional[int] = None,
                              event_detail: Optional[int] = None) -> bool:
        """Mark a command as confirmed/failed/expired/cancelled.

        Args:
            command_id: Command id
            status: New status ('confirmed' | 'failed' | 'expired' | 'cancelled')
            timestamp: When the transition happened (unix seconds). Defaults to
                       now. Stored in confirmed_at for any terminal status.
            event_code: Confirming/failing event code, for audit
            event_detail: Confirming/failing event detail
        """
        ts = timestamp if timestamp is not None else int(time.time())
        try:
            with self._get_connection() as conn:
                conn.execute("""
                    UPDATE node_commands
                    SET status = ?, confirmed_at = ?,
                        confirming_event_code = ?,
                        confirming_event_detail = ?
                    WHERE id = ?
                """, (status, ts, event_code, event_detail, command_id))
            return True
        except duckdb.Error as e:
            logger.error(f"Failed to update command {command_id}: {e}")
            return False

    def query_commands(self, device_id: int,
                       status: Optional[list[str]] = None,
                       since: Optional[int] = None,
                       limit: int = 100) -> list[dict]:
        """Query commands for a device, newest first.

        Args:
            device_id: Device identifier
            status: Optional list of statuses to include (default: all)
            since: Earliest created_at to include (unix seconds)
            limit: Maximum results, capped at 200

        Returns:
            List of command dicts.
        """
        try:
            with self._get_connection() as conn:
                conditions = ["device_id = ?"]
                params: list = [device_id]
                if status:
                    placeholders = ", ".join("?" for _ in status)
                    conditions.append(f"status IN ({placeholders})")
                    params.extend(status)
                if since is not None:
                    conditions.append("created_at >= ?")
                    params.append(since)
                params.append(min(limit, 200))
                where = " AND ".join(conditions)
                result = conn.execute(f"""
                    SELECT id, device_id, command_type, params, status,
                           created_at, confirmed_at, expires_at,
                           huey_task_id, confirming_event_code,
                           confirming_event_detail
                    FROM node_commands
                    WHERE {where}
                    ORDER BY created_at DESC
                    LIMIT ?
                """, params)
                out = []
                for row in result.fetchall():
                    try:
                        row_params = json.loads(row[3])
                    except (ValueError, TypeError):
                        row_params = {}
                    out.append({
                        'id': row[0],
                        'device_id': str(row[1]),
                        'command_type': row[2],
                        'params': row_params,
                        'status': row[4],
                        'created_at': row[5],
                        'confirmed_at': row[6],
                        'expires_at': row[7],
                        'huey_task_id': row[8],
                        'confirming_event_code': row[9],
                        'confirming_event_detail': row[10],
                    })
                return out
        except duckdb.Error as e:
            logger.error(f"Failed to query commands: {e}")
            return []

    def expire_stale_commands(self, now: Optional[int] = None) -> int:
        """Mark any still-pending commands past their TTL as 'expired'.

        Returns the number of rows updated.
        """
        ts = now if now is not None else int(time.time())
        try:
            with self._get_connection() as conn:
                conn.execute("""
                    UPDATE node_commands
                    SET status = 'expired',
                        confirmed_at = ?
                    WHERE status = 'pending'
                      AND expires_at < ?
                """, (ts, ts))
                # DuckDB doesn't expose rowcount on UPDATE reliably; query the
                # count of just-expired rows instead.
                result = conn.execute("""
                    SELECT COUNT(*) FROM node_commands
                    WHERE status = 'expired' AND confirmed_at = ?
                """, (ts,)).fetchone()
                return int(result[0]) if result else 0
        except duckdb.Error as e:
            logger.error(f"Failed to expire stale commands: {e}")
            return 0

    def store_schedule(self, device_id: int, index: int, hour: int, minute: int,
                       duration: int, days: int, valve: int) -> bool:
        """Store an irrigation schedule entry (upsert).

        Args:
            device_id: Device identifier
            index: Schedule index (0-7)
            hour: Hour (0-23)
            minute: Minute (0-59)
            duration: Duration in seconds
            days: Day-of-week bitmask (127 = all days)
            valve: Valve index

        Returns:
            True if stored successfully
        """
        try:
            now = int(time.time())
            with self._get_connection() as conn:
                conn.execute("""
                    INSERT INTO irrigation_schedules
                        (device_id, schedule_index, hour, minute, duration, days, valve, created_at,
                         status, confirmed_at)
                    VALUES (?, ?, ?, ?, ?, ?, ?, ?, 'pending', NULL)
                    ON CONFLICT (device_id, schedule_index)
                    DO UPDATE SET hour = ?, minute = ?, duration = ?, days = ?, valve = ?,
                                  created_at = ?, status = 'pending', confirmed_at = NULL
                """, (device_id, index, hour, minute, duration, days, valve, now,
                      hour, minute, duration, days, valve, now))
                return True
        except duckdb.Error as e:
            logger.error(f"Failed to store schedule: {e}")
            return False

    def get_schedules(self, device_id: int) -> list[dict]:
        """Get all irrigation schedules for a device.

        Args:
            device_id: Device identifier

        Returns:
            List of schedule dicts
        """
        try:
            with self._get_connection() as conn:
                result = conn.execute("""
                    SELECT schedule_index, hour, minute, duration, days, valve, created_at,
                           status, confirmed_at
                    FROM irrigation_schedules
                    WHERE device_id = ?
                    ORDER BY schedule_index
                """, (device_id,))
                rows = result.fetchall()
                return [{
                    'index': row[0],
                    'hour': row[1],
                    'minute': row[2],
                    'duration': row[3],
                    'days': row[4],
                    'valve': row[5],
                    'created_at': row[6],
                    'status': row[7] or 'pending',
                    'confirmed_at': row[8],
                } for row in rows]
        except duckdb.Error as e:
            logger.error(f"Failed to get schedules: {e}")
            return []

    def delete_schedule(self, device_id: int, index: int) -> bool:
        """Delete an irrigation schedule entry.

        Args:
            device_id: Device identifier
            index: Schedule index (0-7)

        Returns:
            True if deleted successfully
        """
        try:
            with self._get_connection() as conn:
                conn.execute("""
                    DELETE FROM irrigation_schedules
                    WHERE device_id = ? AND schedule_index = ?
                """, (device_id, index))
                return True
        except duckdb.Error as e:
            logger.error(f"Failed to delete schedule: {e}")
            return False

    def confirm_schedule(self, device_id: int, schedule_index: int,
                         timestamp: int) -> bool:
        """Mark a schedule as confirmed by the node.

        Args:
            device_id: Device identifier
            schedule_index: Schedule index confirmed by node
            timestamp: Unix timestamp of confirmation

        Returns:
            True if updated successfully
        """
        try:
            with self._get_connection() as conn:
                conn.execute("""
                    UPDATE irrigation_schedules
                    SET status = 'confirmed', confirmed_at = ?
                    WHERE device_id = ? AND schedule_index = ?
                """, (timestamp, device_id, schedule_index))
                return True
        except duckdb.Error as e:
            logger.error(f"Failed to confirm schedule: {e}")
            return False

    def confirm_schedule_removed(self, device_id: int, schedule_index: int,
                                 timestamp: int) -> bool:
        """Remove a schedule after node confirms removal.

        Args:
            device_id: Device identifier
            schedule_index: Schedule index removed by node
            timestamp: Unix timestamp of removal

        Returns:
            True if deleted successfully
        """
        try:
            with self._get_connection() as conn:
                conn.execute("""
                    DELETE FROM irrigation_schedules
                    WHERE device_id = ? AND schedule_index = ?
                """, (device_id, schedule_index))
                return True
        except duckdb.Error as e:
            logger.error(f"Failed to confirm schedule removal: {e}")
            return False

    def fail_schedule(self, device_id: int, schedule_index: int,
                      timestamp: int) -> bool:
        """Mark a schedule as failed on the node.

        Args:
            device_id: Device identifier
            schedule_index: Schedule index that failed
            timestamp: Unix timestamp of failure

        Returns:
            True if updated successfully
        """
        try:
            with self._get_connection() as conn:
                conn.execute("""
                    UPDATE irrigation_schedules
                    SET status = 'failed', confirmed_at = ?
                    WHERE device_id = ? AND schedule_index = ?
                """, (timestamp, device_id, schedule_index))
                return True
        except duckdb.Error as e:
            logger.error(f"Failed to mark schedule as failed: {e}")
            return False
