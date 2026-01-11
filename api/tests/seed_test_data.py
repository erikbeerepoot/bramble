#!/usr/bin/env python3
"""Seed the database with test sensor data for local development."""
import sys
import time
import random
import argparse
import os
from pathlib import Path

# Add parent directory (api/) to path for imports
sys.path.insert(0, str(Path(__file__).parent.parent))

# Use local test DB by default when running this script
os.environ.setdefault('SENSOR_DB_PATH', './test_sensor.db')

from database import SensorDatabase, SensorReading


def seed_database(db: SensorDatabase, node_address: int, num_readings: int, interval_seconds: int = 30):
    """Insert test readings for a node.

    Args:
        db: Database instance
        node_address: Node address to simulate
        num_readings: Number of readings to insert
        interval_seconds: Time between readings (simulated)
    """
    base_time = int(time.time()) - (num_readings * interval_seconds)
    base_temp = 2200  # 22.00°C
    base_humidity = 6500  # 65.00%

    readings_inserted = 0
    for i in range(num_readings):
        # Add some realistic variation
        temp = base_temp + random.randint(-200, 200)  # ±2°C
        humidity = base_humidity + random.randint(-500, 500)  # ±5%
        timestamp = base_time + (i * interval_seconds)

        reading = SensorReading(
            node_address=node_address,
            timestamp=timestamp,
            temperature_centidegrees=temp,
            humidity_centipercent=humidity,
            received_at=timestamp + random.randint(0, 2)  # Small delay
        )

        try:
            db.insert_reading(reading)
            readings_inserted += 1
        except Exception as e:
            # Skip duplicates
            if 'UNIQUE constraint' not in str(e):
                print(f"Error inserting reading: {e}")

    return readings_inserted


def main():
    parser = argparse.ArgumentParser(description='Seed test sensor data')
    parser.add_argument('--db', default='./test_sensor.db', help='Database path')
    parser.add_argument('--node', type=int, default=0x02, help='Node address (default: 2)')
    parser.add_argument('--count', type=int, default=100, help='Number of readings (default: 100)')
    parser.add_argument('--interval', type=int, default=30, help='Seconds between readings (default: 30)')
    parser.add_argument('--reset', action='store_true', help='Delete existing data first')
    args = parser.parse_args()

    os.environ['SENSOR_DB_PATH'] = args.db

    db = SensorDatabase(args.db)
    db.init_db()

    if args.reset:
        # Quick reset - delete all readings
        with db._get_connection() as conn:
            conn.execute('DELETE FROM sensor_readings')
            conn.execute('DELETE FROM nodes')
            conn.commit()
        print(f"Cleared existing data from {args.db}")

    print(f"Seeding {args.count} readings for node {args.node:#04x}...")
    inserted = seed_database(db, args.node, args.count, args.interval)
    print(f"Inserted {inserted} readings into {args.db}")

    # Show summary
    stats = db.get_node_statistics(args.node)
    if stats:
        print(f"\nNode {args.node:#04x} statistics:")
        print(f"  Total readings: {stats['total_readings']}")
        temp = stats['temperature']
        hum = stats['humidity']
        print(f"  Temp range: {temp['min_celsius']:.1f}°C - {temp['max_celsius']:.1f}°C")
        print(f"  Humidity range: {hum['min_percent']:.1f}% - {hum['max_percent']:.1f}%")


if __name__ == '__main__':
    main()
