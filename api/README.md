# Bramble REST API

REST API for managing irrigation schedules over LoRa. Runs on Raspberry Pi Zero 2 W and communicates with the LoRa hub via serial.

## Requirements

- Raspberry Pi Zero 2 W (or any RPi with serial port)
- Python 3.11+
- uv package manager
- LoRa hub connected via USB serial

## Installation

### Option 1: Docker (Recommended)

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
| `API_TOKEN` | `` (empty) | Bearer token required on mutating valve endpoints. Empty disables enforcement. See [docs/API.md](docs/API.md#authentication). |
| `RACHIO_WEBHOOK_SECRET` | `` (empty) | Shared secret authenticating the public Rachio webhook (matched against the payload `externalId`). Empty disables the integration. See [Rachio integration](#rachio-integration). |

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

**Response:**
```json
{
  "status": "queued",
  "node_address": 42,
  "schedule": { ... },
  "position": 1
}
```

### Remove Schedule

**DELETE** `/api/nodes/{addr}/schedules/{index}`

```json
{
  "status": "queued",
  "node_address": 42,
  "schedule_index": 0,
  "position": 2
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

**Response:**
```json
{
  "status": "queued",
  "node_address": 42,
  "interval_seconds": 60,
  "position": 3
}
```

## Rachio Integration

Bramble can mirror an existing Rachio irrigation controller: when a Rachio zone
starts, a mapped Bramble valve runs automatically (and stops when the Rachio zone
stops). No app interaction — a saved mapping drives the action.

### How it works

1. Rachio POSTs a `ZONE_STATUS` webhook to
   `POST /api/integrations/rachio/webhook` on each zone start/stop.
2. The endpoint is **public** (Rachio can't send our bearer token or pass
   Cloudflare Access). It authenticates by comparing the payload's `externalId`
   against `RACHIO_WEBHOOK_SECRET`, so **the webhook path must be excluded from
   Cloudflare Access** at the edge.
3. On `ZONE_STARTED`, the mapped Bramble valve runs for Rachio's reported
   duration (falling back to the mapping's `duration_seconds`); on
   `ZONE_STOPPED` / `ZONE_COMPLETED` it stops. Group master valves mirror the
   run, same as a manual valve command.

### Setup

1. Set `RACHIO_WEBHOOK_SECRET` in the API container env to a strong random string.
2. Find your Rachio controller id and zone numbers, then register the webhook:
   ```bash
   export RACHIO_API_TOKEN=<your-rachio-api-key>   # app.rach.io → Account → Get API key
   python scripts/register_rachio_webhook.py list   # shows controllers, zones, event types
   python scripts/register_rachio_webhook.py register \
       --device-id <controller-uuid> \
       --url https://api.bramble.ag/api/integrations/rachio/webhook \
       --secret "$RACHIO_WEBHOOK_SECRET"
   ```
3. Add a zone → valve mapping (repeat per zone):
   ```bash
   curl -X POST https://api.bramble.ag/api/integrations/rachio/mappings \
     -H "Authorization: Bearer $API_TOKEN" -H "Content-Type: application/json" \
     -d '{"rachio_device_id":"<controller-uuid>","rachio_zone_number":1,
          "bramble_device_id":"1234567890","bramble_valve":0,"duration_seconds":900}'
   ```

### Mapping endpoints

| Method | Path | Auth | Description |
|--------|------|------|-------------|
| `GET` | `/api/integrations/rachio/mappings` | — | List all zone → valve mappings |
| `POST` | `/api/integrations/rachio/mappings` | Bearer | Create/update a mapping (upsert on `rachio_device_id` + `rachio_zone_number`) |
| `DELETE` | `/api/integrations/rachio/mappings/{rachio_device_id}/{rachio_zone_number}` | Bearer | Delete a mapping |

Zones without an enabled mapping are logged and ignored.

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
