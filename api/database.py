"""DuckDB database for sensor data storage."""
import duckdb
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
            'device_id': self.device_id,
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
        return {
            'timestamp': self.timestamp,
            'temperature_celsius': self.temperature_celsius,
            'humidity_percent': self.humidity_percent,
        }


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
        total_readings INTEGER DEFAULT 0
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
        self._conn_lock = threading.Lock()

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
        """Initialize ID counter from existing data."""
        result = conn.execute("SELECT MAX(id) FROM sensor_readings").fetchone()
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
            # Execute schema statements one at a time
            for statement in self.SCHEMA.split(';'):
                statement = statement.strip()
                if statement:
                    conn.execute(statement)
            self._init_id_counter(conn)
            logger.info(f"Database initialized at {self.db_path}")

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
                    COUNT(*) as sample_count
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
                'device_id': row[0],
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
                'device_id': row[0],
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
                    'device_id': row[0],
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
                'device_id': row[0],
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
                    'device_id': row[0],
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
                SELECT device_id, address, node_type, first_seen_at, last_seen_at, total_readings
                FROM nodes WHERE device_id = ?
            """, (device_id,))
            row = result.fetchone()

            if not row:
                return None

            return {
                'device_id': row[0],
                'address': row[1],
                'node_type': row[2],
                'first_seen_at': row[3],
                'last_seen_at': row[4],
                'total_readings': row[5]
            }
