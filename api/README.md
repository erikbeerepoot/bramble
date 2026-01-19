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
docker compose up

# API runs on http://localhost:5000
# Code changes trigger automatic reload
```

**Production mode:**
```bash
# Edit docker-compose.yml: change target to 'production'
docker compose up -d
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
docker compose up  # Starts api + worker
```

**Local testing** can use synchronous mode:
```bash
QUEUE_IMMEDIATE=true uv run python app.py
```

## API Reference

See [docs/API.md](docs/API.md) for complete API documentation.

**Quick examples:**
```bash
# List all nodes
curl http://localhost:5000/api/nodes

# Add irrigation schedule
curl -X POST http://localhost:5000/api/nodes/42/schedules \
  -H "Content-Type: application/json" \
  -d '{"index": 0, "hour": 14, "minute": 30, "duration": 900, "days": 127, "valve": 0}'

# Check task status
curl http://localhost:5000/api/tasks/{task_id}
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
