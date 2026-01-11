"""SQLite database for sensor data storage."""
import sqlite3
import logging
import time
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


class SensorDatabase:
    """SQLite database manager for sensor readings."""

    SCHEMA = """
    CREATE TABLE IF NOT EXISTS sensor_readings (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        node_address INTEGER NOT NULL,
        timestamp INTEGER NOT NULL,
        temperature_centidegrees INTEGER NOT NULL,
        humidity_centipercent INTEGER NOT NULL,
        flags INTEGER DEFAULT 0,
        received_at INTEGER NOT NULL,
        created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
        UNIQUE(node_address, timestamp)
    );

    CREATE INDEX IF NOT EXISTS idx_node_timestamp
        ON sensor_readings(node_address, timestamp);

    CREATE INDEX IF NOT EXISTS idx_timestamp
        ON sensor_readings(timestamp);

    CREATE TABLE IF NOT EXISTS nodes (
        address INTEGER PRIMARY KEY,
        node_type TEXT NOT NULL,
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
            db_path: Path to SQLite database file
        """
        self.db_path = db_path
        self._ensure_directory()

    def _ensure_directory(self):
        """Ensure the database directory exists."""
        db_dir = Path(self.db_path).parent
        db_dir.mkdir(parents=True, exist_ok=True)

    @contextmanager
    def _get_connection(self):
        """Context manager for database connections."""
        conn = sqlite3.connect(self.db_path)
        conn.row_factory = sqlite3.Row
        try:
            yield conn
            conn.commit()
        except Exception:
            conn.rollback()
            raise
        finally:
            conn.close()

    def init_db(self):
        """Initialize database schema."""
        with self._get_connection() as conn:
            conn.executescript(self.SCHEMA)
            logger.info(f"Database initialized at {self.db_path}")

    def insert_reading(self, reading: SensorReading) -> bool:
        """Insert a single sensor reading.

        Args:
            reading: SensorReading to insert

        Returns:
            True if inserted, False if duplicate
        """
        received_at = reading.received_at or int(time.time())

        try:
            with self._get_connection() as conn:
                conn.execute("""
                    INSERT OR IGNORE INTO sensor_readings
                    (node_address, timestamp, temperature_centidegrees,
                     humidity_centipercent, flags, received_at)
                    VALUES (?, ?, ?, ?, ?, ?)
                """, (
                    reading.node_address,
                    reading.timestamp,
                    reading.temperature_centidegrees,
                    reading.humidity_centipercent,
                    reading.flags,
                    received_at
                ))

                # Update node statistics
                self._update_node_stats(conn, reading.node_address, reading.timestamp)

                return conn.total_changes > 0

        except sqlite3.Error as e:
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
                    cursor = conn.execute("""
                        INSERT OR IGNORE INTO sensor_readings
                        (node_address, timestamp, temperature_centidegrees,
                         humidity_centipercent, flags, received_at)
                        VALUES (?, ?, ?, ?, ?, ?)
                    """, (
                        reading.node_address,
                        reading.timestamp,
                        reading.temperature_centidegrees,
                        reading.humidity_centipercent,
                        reading.flags,
                        ts
                    ))

                    if cursor.rowcount > 0:
                        inserted += 1
                        self._update_node_stats(conn, reading.node_address, reading.timestamp)
                    else:
                        duplicates += 1

                logger.info(f"Batch insert: {inserted} inserted, {duplicates} duplicates")
                return (inserted, duplicates)

        except sqlite3.Error as e:
            logger.error(f"Batch insert failed: {e}")
            return (0, len(readings))

    def _update_node_stats(self, conn: sqlite3.Connection, node_address: int, timestamp: int):
        """Update node statistics after inserting a reading."""
        conn.execute("""
            INSERT INTO nodes (address, node_type, first_seen_at, last_seen_at, total_readings)
            VALUES (?, 'SENSOR', ?, ?, 1)
            ON CONFLICT(address) DO UPDATE SET
                last_seen_at = MAX(last_seen_at, excluded.last_seen_at),
                total_readings = total_readings + 1
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
            cursor = conn.execute(f"""
                SELECT node_address, timestamp, temperature_centidegrees,
                       humidity_centipercent, flags, received_at
                FROM sensor_readings
                WHERE {where_clause}
                ORDER BY timestamp DESC
                LIMIT ? OFFSET ?
            """, params)

            return [
                SensorReading(
                    node_address=row['node_address'],
                    timestamp=row['timestamp'],
                    temperature_centidegrees=row['temperature_centidegrees'],
                    humidity_centipercent=row['humidity_centipercent'],
                    flags=row['flags'],
                    received_at=row['received_at']
                )
                for row in cursor.fetchall()
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
            cursor = conn.execute("""
                SELECT address, node_type, first_seen_at, last_seen_at, total_readings
                FROM nodes WHERE address = ?
            """, (node_address,))
            row = cursor.fetchone()

            if not row:
                return None

            # Get temperature stats
            cursor = conn.execute("""
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
            stats = cursor.fetchone()

            return {
                'address': row['address'],
                'node_type': row['node_type'],
                'first_seen_at': row['first_seen_at'],
                'last_seen_at': row['last_seen_at'],
                'total_readings': row['total_readings'],
                'temperature': {
                    'min_celsius': stats['min_temp'] / 100.0 if stats['min_temp'] else None,
                    'max_celsius': stats['max_temp'] / 100.0 if stats['max_temp'] else None,
                    'avg_celsius': stats['avg_temp'] / 100.0 if stats['avg_temp'] else None,
                },
                'humidity': {
                    'min_percent': stats['min_hum'] / 100.0 if stats['min_hum'] else None,
                    'max_percent': stats['max_hum'] / 100.0 if stats['max_hum'] else None,
                    'avg_percent': stats['avg_hum'] / 100.0 if stats['avg_hum'] else None,
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
            cursor = conn.execute(f"""
                SELECT COUNT(*) as count FROM sensor_readings WHERE {where_clause}
            """, params)
            return cursor.fetchone()['count']

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
