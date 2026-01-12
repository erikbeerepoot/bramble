# S3 Persistence Plan

## Overview

Add optional S3 backup for the DuckDB sensor database. Local persistence remains primary - S3 is fire-and-forget backup.

## Design Principles

1. **Local-first**: S3 failure never affects local operation
2. **Simple**: Just upload the whole .duckdb file periodically
3. **Secure**: IAM role with minimal permissions, no credentials in code

## Implementation

### 1. S3 Sync Module

```python
# api/s3_sync.py
import boto3
import logging
from pathlib import Path
from botocore.exceptions import ClientError

logger = logging.getLogger(__name__)

class S3Sync:
    """Optional S3 backup for sensor database."""

    def __init__(self, bucket: str, key_prefix: str = "bramble/"):
        self.bucket = bucket
        self.key_prefix = key_prefix
        self.client = boto3.client('s3')

    def upload(self, db_path: str) -> bool:
        """Upload database file to S3. Returns True on success."""
        try:
            key = f"{self.key_prefix}sensor_data.duckdb"
            self.client.upload_file(db_path, self.bucket, key)
            logger.info(f"Uploaded {db_path} to s3://{self.bucket}/{key}")
            return True
        except ClientError as e:
            logger.warning(f"S3 upload failed (non-fatal): {e}")
            return False
```

### 2. Config

```python
# config.py additions
S3_BUCKET = os.getenv('S3_BUCKET', '')  # Empty = disabled
S3_KEY_PREFIX = os.getenv('S3_KEY_PREFIX', 'bramble/')
S3_SYNC_INTERVAL = int(os.getenv('S3_SYNC_INTERVAL', '3600'))  # 1 hour default
```

### 3. Periodic Sync

Call `s3_sync.upload()` on a schedule (e.g., hourly via cron or background thread). Failure is logged but ignored.

## Security

### IAM Policy (minimal permissions)
```json
{
    "Version": "2012-10-17",
    "Statement": [
        {
            "Effect": "Allow",
            "Action": [
                "s3:PutObject"
            ],
            "Resource": "arn:aws:s3:::YOUR-BUCKET/bramble/*"
        }
    ]
}
```

### Credential Options (best to worst)

1. **IAM Role for EC2/IoT** (best) - No credentials stored, auto-rotated
2. **IAM Role Anywhere** - For on-prem devices with X.509 certs
3. **Environment variables** - `AWS_ACCESS_KEY_ID`, `AWS_SECRET_ACCESS_KEY`
4. **~/.aws/credentials** file (avoid on shared devices)

### S3 Bucket Security

- Enable **default encryption** (SSE-S3 or SSE-KMS)
- Enable **versioning** (recover from corruption)
- Block public access
- Consider **Object Lock** for compliance

## Tasks

- [ ] Add `boto3` to pyproject.toml
- [ ] Create `s3_sync.py` module
- [ ] Add S3 config options to `config.py`
- [ ] Add hourly sync (cron job or background thread)
- [ ] Document IAM policy and bucket setup in README

## File Changes

| File | Change |
|------|--------|
| `api/s3_sync.py` | New: S3 upload module |
| `api/config.py` | Add S3 settings |
| `api/pyproject.toml` | Add boto3 dependency |
| `api/README.md` | Document S3 setup |
