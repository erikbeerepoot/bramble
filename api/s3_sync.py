#!/usr/bin/env python3
"""CLI script to sync sensor database to S3. Run via cron for daily backup."""
import sys
import logging

from config import Config
from database import SensorDatabase

logging.basicConfig(
    level=logging.INFO,
    format='%(asctime)s - %(levelname)s - %(message)s'
)

def main():
    if not Config.S3_BUCKET:
        print("S3_BUCKET not configured, skipping sync")
        return 0

    db = SensorDatabase()
    success = db.sync_to_s3()
    return 0 if success else 1

if __name__ == '__main__':
    sys.exit(main())
