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
    node_address: int
    timestamp: int  # Unix timestamp in seconds
    temperature_centidegrees: int  # Temperature in 0.01C units
    humidity_centipercent: int  # Humidity in 0.01% units
    flags: int = 0
    received_at: Optional[int] = None  # When hub received it

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
            'node_address': self.node_address,
            'timestamp': self.timestamp,
            'temperature_celsius': self.temperature_celsius,
            'humidity_percent': self.humidity_percent,
            'temperature_raw': self.temperature_centidegrees,
            'humidity_raw': self.humidity_centipercent,
            'flags': self.flags,
            'received_at': self.received_at
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
        node_address INTEGER NOT NULL,
        timestamp INTEGER NOT NULL,
        temperature_centidegrees INTEGER NOT NULL,
        humidity_centipercent INTEGER NOT NULL,
        flags INTEGER DEFAULT 0,
        received_at INTEGER NOT NULL,
        created_at TIMESTAMP DEFAULT current_timestamp,
        UNIQUE(node_address, timestamp)
    );

    CREATE INDEX IF NOT EXISTS idx_node_timestamp
        ON sensor_readings(node_address, timestamp);

    CREATE INDEX IF NOT EXISTS idx_timestamp
        ON sensor_readings(timestamp);

    CREATE TABLE IF NOT EXISTS nodes (
        address INTEGER PRIMARY KEY,
        node_type VARCHAR NOT NULL,
        first_seen_at INTEGER NOT NULL,
        last_seen_at INTEGER NOT NULL,
        total_readings INTEGER DEFAULT 0
    );

    CREATE INDEX IF NOT EXISTS idx_last_seen
        ON nodes(last_seen_at);
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

    @contextmanager
    def _get_connection(self):
        """Context manager for database connections."""
        conn = duckdb.connect(self.db_path)
        try:
            yield conn
        finally:
            conn.close()

    def init_db(self):
        """Initialize database schema."""
        with self._get_connection() as conn:
            # Execute schema statements one at a time
            for statement in self.SCHEMA.split(';'):
                statement = statement.strip()
                if statement:
                    conn.execute(statement)
            self._init_id_counter(conn)
            logger.info(f"Database initialized at {self.db_path}")

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
                    (id, node_address, timestamp, temperature_centidegrees,
                     humidity_centipercent, flags, received_at)
                    VALUES (?, ?, ?, ?, ?, ?, ?)
                    ON CONFLICT (node_address, timestamp) DO NOTHING
                """, (
                    new_id,
                    reading.node_address,
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
                    self._update_node_stats(conn, reading.node_address, reading.timestamp)
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
                        (id, node_address, timestamp, temperature_centidegrees,
                         humidity_centipercent, flags, received_at)
                        VALUES (?, ?, ?, ?, ?, ?, ?)
                        ON CONFLICT (node_address, timestamp) DO NOTHING
                    """, (
                        new_id,
                        reading.node_address,
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
                        self._update_node_stats(conn, reading.node_address, reading.timestamp)
                    else:
                        duplicates += 1

                logger.info(f"Batch insert: {inserted} inserted, {duplicates} duplicates")
                return (inserted, duplicates)

        except duckdb.Error as e:
            logger.error(f"Batch insert failed: {e}")
            return (0, len(readings))

    def _update_node_stats(self, conn: duckdb.DuckDBPyConnection, node_address: int, timestamp: int):
        """Update node statistics after inserting a reading."""
        # Check if node exists
        existing = conn.execute(
            "SELECT address, first_seen_at, total_readings FROM nodes WHERE address = ?",
            (node_address,)
        ).fetchone()

        if existing:
            conn.execute("""
                UPDATE nodes SET
                    last_seen_at = GREATEST(last_seen_at, ?),
                    total_readings = total_readings + 1
                WHERE address = ?
            """, (timestamp, node_address))
        else:
            conn.execute("""
                INSERT INTO nodes (address, node_type, first_seen_at, last_seen_at, total_readings)
                VALUES (?, 'SENSOR', ?, ?, 1)
            """, (node_address, timestamp, timestamp))

    def query_readings(
        self,
        node_address: Optional[int] = None,
        start_time: Optional[int] = None,
        end_time: Optional[int] = None,
        limit: int = 1000,
        offset: int = 0
    ) -> list[SensorReading]:
        """Query sensor readings with filters.

        Args:
            node_address: Filter by node address (optional)
            start_time: Start timestamp (optional)
            end_time: End timestamp (optional)
            limit: Maximum records to return
            offset: Number of records to skip

        Returns:
            List of SensorReading objects
        """
        conditions = []
        params = []

        if node_address is not None:
            conditions.append("node_address = ?")
            params.append(node_address)
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
                SELECT node_address, timestamp, temperature_centidegrees,
                       humidity_centipercent, flags, received_at
                FROM sensor_readings
                WHERE {where_clause}
                ORDER BY timestamp DESC
                LIMIT ? OFFSET ?
            """, params)

            return [
                SensorReading(
                    node_address=row[0],
                    timestamp=row[1],
                    temperature_centidegrees=row[2],
                    humidity_centipercent=row[3],
                    flags=row[4],
                    received_at=row[5]
                )
                for row in result.fetchall()
            ]

    def get_latest_reading(self, node_address: int) -> Optional[SensorReading]:
        """Get the most recent reading for a node.

        Args:
            node_address: Node address to query

        Returns:
            Latest SensorReading or None
        """
        readings = self.query_readings(node_address=node_address, limit=1)
        return readings[0] if readings else None

    def get_node_statistics(self, node_address: int) -> Optional[dict]:
        """Get statistics for a specific node.

        Args:
            node_address: Node address to query

        Returns:
            Dictionary with node statistics or None
        """
        with self._get_connection() as conn:
            # Get node info
            result = conn.execute("""
                SELECT address, node_type, first_seen_at, last_seen_at, total_readings
                FROM nodes WHERE address = ?
            """, (node_address,))
            row = result.fetchone()

            if not row:
                return None

            # Get temperature stats
            stats_result = conn.execute("""
                SELECT
                    MIN(temperature_centidegrees) as min_temp,
                    MAX(temperature_centidegrees) as max_temp,
                    AVG(temperature_centidegrees) as avg_temp,
                    MIN(humidity_centipercent) as min_hum,
                    MAX(humidity_centipercent) as max_hum,
                    AVG(humidity_centipercent) as avg_hum
                FROM sensor_readings
                WHERE node_address = ?
            """, (node_address,))
            stats = stats_result.fetchone()

            return {
                'address': row[0],
                'node_type': row[1],
                'first_seen_at': row[2],
                'last_seen_at': row[3],
                'total_readings': row[4],
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
        node_address: Optional[int] = None,
        start_time: Optional[int] = None,
        end_time: Optional[int] = None
    ) -> int:
        """Get count of readings matching filters.

        Args:
            node_address: Filter by node address (optional)
            start_time: Start timestamp (optional)
            end_time: End timestamp (optional)

        Returns:
            Count of matching readings
        """
        conditions = []
        params = []

        if node_address is not None:
            conditions.append("node_address = ?")
            params.append(node_address)
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

    def export_csv(
        self,
        node_address: Optional[int] = None,
        start_time: Optional[int] = None,
        end_time: Optional[int] = None
    ) -> str:
        """Export readings as CSV string.

        Args:
            node_address: Filter by node address (optional)
            start_time: Start timestamp (optional)
            end_time: End timestamp (optional)

        Returns:
            CSV formatted string
        """
        readings = self.query_readings(
            node_address=node_address,
            start_time=start_time,
            end_time=end_time,
            limit=100000  # Higher limit for export
        )

        lines = ["node_address,timestamp,temperature_celsius,humidity_percent,flags"]
        for r in readings:
            lines.append(f"{r.node_address},{r.timestamp},{r.temperature_celsius:.2f},{r.humidity_percent:.2f},{r.flags}")

        return "\n".join(lines)

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

                # Also export nodes table
                nodes_path = f"s3://{bucket}/{prefix}nodes.parquet"
                conn.execute(f"COPY nodes TO '{nodes_path}' (FORMAT PARQUET)")

                logger.info(f"Synced database to s3://{bucket}/{prefix}")
                return True

        except Exception as e:
            logger.warning(f"S3 sync failed (non-fatal): {e}")
            return False
