"""Configuration for Bramble REST API."""
import os

class Config:
    """Application configuration."""

    # Flask settings
    DEBUG = os.getenv('DEBUG', 'False').lower() == 'true'
    HOST = os.getenv('HOST', '0.0.0.0')
    PORT = int(os.getenv('PORT', '5000'))

    # Serial settings
    SERIAL_PORT = os.getenv('SERIAL_PORT', '/dev/ttyAMA0')
    SERIAL_BAUD = int(os.getenv('SERIAL_BAUD', '115200'))
    SERIAL_TIMEOUT = float(os.getenv('SERIAL_TIMEOUT', '1.0'))

    # Hub communication settings
    COMMAND_TIMEOUT = float(os.getenv('COMMAND_TIMEOUT', '5.0'))
    MAX_RETRIES = int(os.getenv('MAX_RETRIES', '3'))

    # Database settings
    SENSOR_DB_PATH = os.getenv('SENSOR_DB_PATH', '/data/sensor_data.duckdb')
    DB_MEMORY_LIMIT = os.getenv('DB_MEMORY_LIMIT', '128MB')  # DuckDB memory limit (default 75% RAM is too aggressive for Pi)
    DB_THREADS = int(os.getenv('DB_THREADS', '2'))  # DuckDB worker threads
    DB_BATCH_SIZE = int(os.getenv('DB_BATCH_SIZE', '100'))  # Records per batch insert
    DB_FLUSH_INTERVAL = float(os.getenv('DB_FLUSH_INTERVAL', '5.0'))  # Seconds between buffer flushes

    # S3 backup settings (optional - leave S3_BUCKET empty to disable)
    S3_BUCKET = os.getenv('S3_BUCKET', '')
    S3_PREFIX = os.getenv('S3_PREFIX', 'bramble/')
    S3_REGION = os.getenv('S3_REGION', 'us-east-1')

    # Command queue settings (huey with SQLite backend)
    QUEUE_DB_PATH: str = os.getenv('QUEUE_DB_PATH', '/data/queue.db')
    QUEUE_IMMEDIATE: bool = os.getenv('QUEUE_IMMEDIATE', 'false').lower() == 'true'
