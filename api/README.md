# Bramble REST API

REST API for managing irrigation schedules over LoRa. Runs on Raspberry Pi Zero 2 W and communicates with the LoRa hub via serial.

## Requirements

- Raspberry Pi Zero 2 W (or any RPi with serial port)
- Python 3.11+
- uv package manager
- LoRa hub connected via USB serial

## Installation

### Option 1: Docker (Recommended)

**Quick Start - Pull pre-built image:**
```bash
# Pull from GitHub Container Registry (works on x86 and Raspberry Pi)
docker pull ghcr.io/erikbeerepoot/bramble/api:latest

# Run with serial device access (Raspberry Pi)
docker run -d \
  --name bramble-api \
  -p 5000:5000 \
  --device /dev/ttyAMA0:/dev/ttyAMA0 \
  -e SERIAL_PORT=/dev/ttyAMA0 \
  -v bramble-data:/app/data \
  ghcr.io/erikbeerepoot/bramble/api:latest

# API runs on http://localhost:5000
```

**Prerequisites - Install Docker on Raspberry Pi/Debian:**

```bash
# Add Docker's official GPG key
sudo apt-get update
sudo apt-get install ca-certificates curl
sudo install -m 0755 -d /etc/apt/keyrings
sudo curl -fsSL https://download.docker.com/linux/debian/gpg -o /etc/apt/keyrings/docker.asc
sudo chmod a+r /etc/apt/keyrings/docker.asc

# Add the repository to Apt sources
echo \
  "deb [arch=$(dpkg --print-architecture) signed-by=/etc/apt/keyrings/docker.asc] https://download.docker.com/linux/debian \
  $(. /etc/os-release && echo "$VERSION_CODENAME") stable" | \
  sudo tee /etc/apt/sources.list.d/docker.list > /dev/null
sudo apt-get update

# Install Docker packages
sudo apt-get install docker-ce docker-ce-cli containerd.io docker-buildx-plugin docker-compose-plugin

# Add user to docker group (avoid sudo)
sudo usermod -aG docker $USER

# Add user to dialout group for serial access
sudo usermod -aG dialout $USER

# Log out and back in for group changes to take effect
```

**Development mode with hot reload:**
```bash
cd bramble/api

# Build and run with hot reload
docker-compose up

# API runs on http://localhost:5000
# Code changes trigger automatic reload
```

**Production mode:**
```bash
# Edit docker-compose.yml: change target to 'production'
docker-compose up -d
```

**Note:**
- Serial device must be available at `/dev/ttyACM0` or `/dev/ttyAMA0` (edit in docker-compose.yml if different)
- **If using `/dev/ttyAMA0` (hardware UART)**: Disable serial console and Bluetooth:
  ```bash
  # Edit boot config
  sudo nvim /boot/firmware/config.txt

  # Add these lines at the end:
  enable_uart=1
  dtoverlay=disable-bt

  # Save and reboot
  sudo reboot
  ```

### Option 2: Native (Direct on Raspberry Pi)

**1. Install uv:**
```bash
curl -LsSf https://astral.sh/uv/install.sh | sh
source $HOME/.cargo/env
```

**2. Clone and setup:**
```bash
cd /home/pi
git clone <bramble-repo> bramble
cd bramble/api
uv sync
```

**3. Configure NTP:**
```bash
sudo timedatectl set-ntp true
timedatectl status  # Verify NTP is active
```

**4. Run the API:**
```bash
# Test run
uv run python app.py

# Install as systemd service
sudo cp systemd/bramble-api.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable bramble-api
sudo systemctl start bramble-api
```

## Configuration

Environment variables:

| Variable | Default | Description |
|----------|---------|-------------|
| `SERIAL_PORT` | `/dev/ttyACM0` | Serial port for hub |
| `SERIAL_BAUD` | `115200` | Baud rate |
| `HOST` | `0.0.0.0` | API server host |
| `PORT` | `5000` | API server port |
| `DEBUG` | `False` | Enable debug logging |
| `QUEUE_DB_PATH` | `/data/queue.db` | Path to command queue SQLite database |
| `QUEUE_IMMEDIATE` | `false` | Run queue synchronously (for testing) |
| `SENSOR_DB_PATH` | `/data/sensor_data.duckdb` | Path to sensor data database |

## Architecture

### Command Queue

Commands to nodes (schedules, wake intervals, datetime) are processed through a persistent queue that survives container restarts:

```
┌─────────────┐     ┌──────────────┐     ┌─────────────────────────┐
│ Flask API   │     │ huey queue   │     │ huey worker (consumer)  │
│             │ ──► │ (SQLite)     │ ──► │                         │
│ POST /...   │     │ /data/queue.db     │ send_hub_command()      │
│ returns 202 │     │              │     │   ├─ serial.send()      │
└─────────────┘     └──────────────┘     │   └─ retry on failure   │
                                         └─────────────────────────┘
```

- Commands return immediately with a `task_id` (HTTP 202 Accepted)
- Worker processes commands asynchronously with retries (3 attempts, 30s delay)
- Queue persists in SQLite, surviving container restarts
- Use `/api/tasks/<task_id>` to check command status

**Docker Compose** runs both the API and worker containers:
```bash
docker-compose up  # Starts api + worker
```

**Local testing** can use synchronous mode:
```bash
QUEUE_IMMEDIATE=true uv run python app.py
```

## API Endpoints

### Health Check

**GET** `/api/health`

```json
{
  "status": "healthy",
  "serial_connected": true,
  "serial_port": "/dev/ttyACM0"
}
```

### System Time

**GET** `/api/system/time`

```json
{
  "datetime": "2025-01-15T14:30:00",
  "formatted": "2025-01-15 14:30:00",
  "weekday": 3,
  "timestamp": 1736953800
}
```

### List Nodes

**GET** `/api/nodes`

```json
{
  "count": 2,
  "nodes": [
    {
      "address": 42,
      "type": "IRRIGATION",
      "online": true,
      "last_seen_seconds": 15
    }
  ]
}
```

### Get Node

**GET** `/api/nodes/{addr}`

```json
{
  "address": 42,
  "type": "IRRIGATION",
  "online": true,
  "last_seen_seconds": 15
}
```

### Get Update Queue

**GET** `/api/nodes/{addr}/queue`

```json
{
  "node_address": 42,
  "count": 2,
  "updates": [
    {
      "sequence": 5,
      "type": "SET_SCHEDULE",
      "age_seconds": 30
    }
  ]
}
```

### Add Schedule

**POST** `/api/nodes/{addr}/schedules`

**Request:**
```json
{
  "index": 0,
  "hour": 14,
  "minute": 30,
  "duration": 900,
  "days": 127,
  "valve": 0
}
```

**Parameters:**
- `index`: Schedule slot (0-7)
- `hour`: Hour (0-23)
- `minute`: Minute (0-59)
- `duration`: Duration in seconds (0-65535)
- `days`: Day bitmask (127 = all days, 1 = Sunday, 2 = Monday, etc.)
- `valve`: Valve ID (0+)

**Response (202 Accepted):**
```json
{
  "status": "queued",
  "task_id": "abc123",
  "node_address": 42,
  "schedule": { ... },
  "message": "Command queued for delivery"
}
```

### Remove Schedule

**DELETE** `/api/nodes/{addr}/schedules/{index}`

**Response (202 Accepted):**
```json
{
  "status": "queued",
  "task_id": "def456",
  "node_address": 42,
  "schedule_index": 0,
  "message": "Command queued for delivery"
}
```

### Set Wake Interval

**POST** `/api/nodes/{addr}/wake-interval`

**Request:**
```json
{
  "interval_seconds": 60
}
```

**Response (202 Accepted):**
```json
{
  "status": "queued",
  "task_id": "ghi789",
  "node_address": 42,
  "interval_seconds": 60,
  "message": "Command queued for delivery"
}
```

### Check Task Status

**GET** `/api/tasks/{task_id}`

Check the status of a queued command.

**Response (pending):**
```json
{
  "status": "pending",
  "task_id": "abc123"
}
```

**Response (completed):**
```json
{
  "status": "completed",
  "task_id": "abc123",
  "result": {
    "status": "success",
    "response": ["QUEUED SET_SCHEDULE 42 1"]
  }
}
```

**Response (failed):**
```json
{
  "status": "failed",
  "task_id": "abc123",
  "error": "Hub timeout"
}
```

## Usage Examples

```bash
# List all nodes
curl http://localhost:5000/api/nodes

# Add irrigation schedule (water at 2:30 PM for 15 minutes, all days, valve 0)
curl -X POST http://localhost:5000/api/nodes/42/schedules \
  -H "Content-Type: application/json" \
  -d '{
    "index": 0,
    "hour": 14,
    "minute": 30,
    "duration": 900,
    "days": 127,
    "valve": 0
  }'

# Remove schedule
curl -X DELETE http://localhost:5000/api/nodes/42/schedules/0

# Set wake interval to 1 minute
curl -X POST http://localhost:5000/api/nodes/42/wake-interval \
  -H "Content-Type: application/json" \
  -d '{"interval_seconds": 60}'

# Check node's update queue
curl http://localhost:5000/api/nodes/42/queue
```

## Troubleshooting

### Serial port not found

```bash
# List available serial ports
ls -l /dev/tty*

# Check if hub is connected
dmesg | grep tty

# Add user to dialout group
sudo usermod -a -G dialout pi
```

### API not responding

```bash
# Check service status
sudo systemctl status bramble-api

# View logs
sudo journalctl -u bramble-api -n 50
```

### Time sync issues

```bash
# Force NTP sync
sudo systemctl restart systemd-timesyncd

# Check sync status
timedatectl status
```
