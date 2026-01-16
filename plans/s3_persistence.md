# S3 Persistence Plan

## Overview

Add optional S3 backup using DuckDB's native S3 support (httpfs extension). Local persistence remains primary.

## Design

DuckDB can write directly to S3 - no boto3 needed. Two options:

### Option A: Export to Parquet (recommended)
```python
conn.execute("INSTALL httpfs; LOAD httpfs;")
conn.execute("COPY sensor_readings TO 's3://bucket/bramble/readings.parquet' (FORMAT PARQUET)")
```
- Parquet is columnar, compresses well (~10-20MB/year)
- Can query directly from S3 later with any tool (Athena, DuckDB, pandas)

### Option B: Full database copy
```python
conn.execute("COPY FROM DATABASE memory TO 's3://bucket/bramble/backup.duckdb'")
```

## Implementation

### 1. Add S3 sync to database.py

```python
def sync_to_s3(self, bucket: str, prefix: str = "bramble/") -> bool:
    """Export sensor data to S3. Returns True on success."""
    if not bucket:
        return False
    try:
        with self._get_connection() as conn:
            conn.execute("INSTALL httpfs; LOAD httpfs;")
            s3_path = f"s3://{bucket}/{prefix}sensor_readings.parquet"
            conn.execute(f"COPY sensor_readings TO '{s3_path}' (FORMAT PARQUET)")
            logger.info(f"Synced to {s3_path}")
            return True
    except Exception as e:
        logger.warning(f"S3 sync failed (non-fatal): {e}")
        return False
```

### 2. Config

```python
# config.py
S3_BUCKET = os.getenv('S3_BUCKET', '')  # Empty = disabled
S3_PREFIX = os.getenv('S3_PREFIX', 'bramble/')
S3_REGION = os.getenv('S3_REGION', 'us-east-1')
```

### 3. Credential Setup

DuckDB httpfs uses standard AWS credential chain:
1. Environment variables (`AWS_ACCESS_KEY_ID`, `AWS_SECRET_ACCESS_KEY`)
2. `~/.aws/credentials` file
3. IAM role (EC2/ECS/Lambda)

For explicit config:
```python
conn.execute(f"SET s3_region='{region}';")
conn.execute(f"SET s3_access_key_id='{key}';")  # Only if not using IAM role
conn.execute(f"SET s3_secret_access_key='{secret}';")
```

## Security

### IAM Policy (minimal)
```json
{
    "Version": "2012-10-17",
    "Statement": [{
        "Effect": "Allow",
        "Action": ["s3:PutObject"],
        "Resource": "arn:aws:s3:::YOUR-BUCKET/bramble/*"
    }]
}
```

### Bucket Config
- Default encryption (SSE-S3)
- Versioning enabled
- Block public access

## Tasks

- [x] Add `sync_to_s3()` method to SensorDatabase
- [x] Add S3 config to config.py
- [x] Add `s3_sync.py` CLI script for cron
- [ ] Document IAM/bucket setup in README

## File Changes

| File | Change |
|------|--------|
| `api/database.py` | Add sync_to_s3() method |
| `api/config.py` | Add S3 settings |
| `api/README.md` | Document S3 setup |

No new dependencies - httpfs is bundled with DuckDB.
