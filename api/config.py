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
    SENSOR_DB_PATH = os.getenv('SENSOR_DB_PATH', '/data/sensor_data.db')
    DB_BATCH_SIZE = int(os.getenv('DB_BATCH_SIZE', '100'))  # Records per batch insert
