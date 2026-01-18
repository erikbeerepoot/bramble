# Command Queue with Huey

## Problem

When the API Docker container restarts, any pending commands (valve switches, schedule updates, etc.) that haven't been sent to the hub are lost. We need reliable, persistent command delivery that survives restarts.

## Solution

Use **huey** with SQLite backend for a local persistent job queue. This gives us:
- SQS-like semantics (retries, delays, visibility timeout)
- Persistence across container restarts
- No additional infrastructure (SQLite file)
- Python-native integration with Flask

## Architecture

```
┌─────────────────────────────────────────────────────────────────────────┐
│  Pi API Container                                                       │
│                                                                         │
│  ┌─────────────┐     ┌──────────────┐     ┌─────────────────────────┐  │
│  │ Flask API   │     │ huey queue   │     │ huey worker (consumer)  │  │
│  │             │ ──► │ (SQLite)     │ ──► │                         │  │
│  │ POST /valve │     │ /data/queue.db     │ send_hub_command()      │  │
│  │ returns 202 │     │              │     │   ├─ serial.send()      │  │
│  └─────────────┘     └──────────────┘     │   ├─ wait for ACK       │  │
│                             │             │   └─ retry on failure   │  │
│                             │             └─────────────────────────┘  │
│                             │                        │                  │
│                      Survives restart                │                  │
│                                                      ▼                  │
│                                              ┌─────────────┐            │
│                                              │ Serial/Hub  │            │
│                                              └─────────────┘            │
└─────────────────────────────────────────────────────────────────────────┘
```

## Current State

- Branch: `feature/command-queue-huey` (created)
- Added `huey>=2.5.0` to `api/pyproject.toml`

## Files to Create/Modify

### 1. Create `api/command_queue.py` (NEW)

```python
"""Persistent command queue using huey with SQLite backend."""
from huey import SqliteHuey
from config import Config
import logging

logger = logging.getLogger(__name__)

# Initialize huey with SQLite backend
huey = SqliteHuey(
    filename=Config.QUEUE_DB_PATH,  # /data/queue.db
    immediate=Config.QUEUE_IMMEDIATE,  # True for testing (sync), False for prod
)

@huey.task(retries=3, retry_delay=30)
def send_hub_command(command: str, command_id: str) -> dict:
    """Send a command to the hub via serial.

    Args:
        command: The serial command string (e.g., "SET_SCHEDULE 1 0 14 30 900 127 0")
        command_id: Unique ID for tracking

    Returns:
        dict with status and response
    """
    from app import get_serial  # Import here to avoid circular imports

    logger.info(f"[{command_id}] Sending command: {command}")

    try:
        serial = get_serial()
        responses = serial.send_command(command)

        if responses and responses[0].startswith('QUEUED'):
            logger.info(f"[{command_id}] Command queued on hub: {responses[0]}")
            return {'status': 'success', 'response': responses}
        else:
            logger.warning(f"[{command_id}] Unexpected response: {responses}")
            return {'status': 'warning', 'response': responses}

    except TimeoutError:
        logger.error(f"[{command_id}] Hub timeout - will retry")
        raise  # Raising causes huey to retry

    except Exception as e:
        logger.error(f"[{command_id}] Failed: {e}")
        raise


# Typed wrapper functions for specific commands
@huey.task(retries=3, retry_delay=30)
def queue_set_schedule(node_addr: int, index: int, hour: int, minute: int,
                       duration: int, days: int, valve: int) -> dict:
    """Queue a SET_SCHEDULE command."""
    command = f"SET_SCHEDULE {node_addr} {index} {hour} {minute} {duration} {days} {valve}"
    return send_hub_command(command, f"schedule-{node_addr}-{index}")


@huey.task(retries=3, retry_delay=30)
def queue_remove_schedule(node_addr: int, index: int) -> dict:
    """Queue a REMOVE_SCHEDULE command."""
    command = f"REMOVE_SCHEDULE {node_addr} {index}"
    return send_hub_command(command, f"remove-schedule-{node_addr}-{index}")


@huey.task(retries=3, retry_delay=30)
def queue_set_wake_interval(node_addr: int, interval_seconds: int) -> dict:
    """Queue a SET_WAKE_INTERVAL command."""
    command = f"SET_WAKE_INTERVAL {node_addr} {interval_seconds}"
    return send_hub_command(command, f"wake-{node_addr}")


@huey.task(retries=3, retry_delay=30)
def queue_set_datetime(node_addr: int, year: int, month: int, day: int,
                       weekday: int, hour: int, minute: int, second: int) -> dict:
    """Queue a SET_DATETIME command."""
    command = f"SET_DATETIME {node_addr} {year} {month} {day} {weekday} {hour} {minute} {second}"
    return send_hub_command(command, f"datetime-{node_addr}")
```

### 2. Update `api/config.py`

Add queue configuration:

```python
# Command queue settings
QUEUE_DB_PATH: str = os.getenv('QUEUE_DB_PATH', '/data/queue.db')
QUEUE_IMMEDIATE: bool = os.getenv('QUEUE_IMMEDIATE', 'false').lower() == 'true'
```

### 3. Update `api/app.py`

Modify command endpoints to use huey queue instead of direct serial calls.

**Before (direct):**
```python
@app.route('/api/nodes/<int:addr>/schedules', methods=['POST'])
def add_schedule(addr: int):
    # ... validation ...
    serial = get_serial()
    responses = serial.send_command(command)
    # Returns immediately after serial response
```

**After (queued):**
```python
@app.route('/api/nodes/<int:addr>/schedules', methods=['POST'])
def add_schedule(addr: int):
    # ... validation ...
    from command_queue import queue_set_schedule

    result = queue_set_schedule(
        node_addr=addr,
        index=data['index'],
        hour=data['hour'],
        minute=data['minute'],
        duration=data['duration'],
        days=data['days'],
        valve=data['valve']
    )

    return jsonify({
        'status': 'queued',
        'task_id': result.id,
        'message': 'Command queued for delivery'
    }), 202  # Accepted
```

### 4. Add task status endpoint to `api/app.py`

```python
@app.route('/api/tasks/<task_id>', methods=['GET'])
def get_task_status(task_id: str):
    """Check status of a queued command."""
    from command_queue import huey

    result = huey.result(task_id)

    if result is None:
        return jsonify({'status': 'pending', 'task_id': task_id})
    elif isinstance(result, Exception):
        return jsonify({'status': 'failed', 'task_id': task_id, 'error': str(result)})
    else:
        return jsonify({'status': 'completed', 'task_id': task_id, 'result': result})
```

### 5. Update `api/Dockerfile`

Add huey worker process (or use supervisord):

```dockerfile
# Option A: Run both in same container with supervisord
CMD ["supervisord", "-c", "/etc/supervisord.conf"]

# Option B: Separate containers (cleaner)
# In docker-compose.yml, add:
#   worker:
#     build: ./api
#     command: huey_consumer command_queue.huey
#     volumes:
#       - ./data:/data
```

### 6. Create `api/supervisord.conf` (if using Option A)

```ini
[supervisord]
nodaemon=true

[program:api]
command=python app.py
autostart=true
autorestart=true

[program:huey]
command=huey_consumer command_queue.huey -w 2 -k thread
autostart=true
autorestart=true
```

### 7. Update `docker-compose.yml`

```yaml
services:
  api:
    build: ./api
    volumes:
      - ./data:/data  # Persists both sensor_data.duckdb and queue.db
    # ... rest of config

  # Option B: Separate worker container
  worker:
    build: ./api
    command: huey_consumer command_queue.huey -w 2 -k thread
    volumes:
      - ./data:/data
    depends_on:
      - api
```

## Implementation Order

1. ✅ Create branch `feature/command-queue-huey`
2. ✅ Add huey to `pyproject.toml`
3. [ ] Add config values to `config.py`
4. [ ] Create `command_queue.py`
5. [ ] Update `app.py` endpoints to use queue
6. [ ] Add `/api/tasks/<id>` status endpoint
7. [ ] Update Docker setup (Dockerfile + docker-compose)
8. [ ] Test locally with `QUEUE_IMMEDIATE=true`
9. [ ] Test with worker process

## Testing

```bash
# Install deps
cd api && uv sync

# Run in immediate mode (synchronous, for testing)
QUEUE_IMMEDIATE=true python app.py

# Run with worker (production mode)
# Terminal 1: API
python app.py

# Terminal 2: Worker
huey_consumer command_queue.huey -w 2 -k thread
```

## Rollback

If issues arise, the endpoints can fall back to direct serial calls by setting `QUEUE_IMMEDIATE=true` (tasks execute synchronously in the API process).
