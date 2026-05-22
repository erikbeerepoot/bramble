---
name: bramble-logs
description: Analyze bramble node logs to identify errors, track communication, and summarize node health.
---

# Bramble Log Analyzer

Analyze logs from bramble nodes (sensor, irrigation, hub) to diagnose issues and monitor system health. Optimized for pasting serial monitor output.

## Arguments

`$ARGUMENTS` - Optional: file path and/or filter flags
- `<filepath>` - Path to log file (if omitted, prompt user to paste logs)
- `--module <name>` - Filter by module (SENSOR, IRRIG, HubMode, PMU, etc.)
- `--level <ERR|WARN|INFO|DBG>` - Minimum log level to show
- `--node <0xXXXX>` - Filter by node address
- `--errors` - Show only errors and warnings
- `--detailed` - Show full detailed analysis

## Instructions

### 1. Obtain Log Content

If a file path is provided in `$ARGUMENTS`:
- Use the Read tool to read the file contents
- If file not found, report error and stop

If no file path provided:
- Ask the user: "Please paste your bramble log output. You can paste multiple lines - just let me know when you're done."
- Wait for the user to provide logs before proceeding with analysis

### 2. Parse Log Format

Bramble firmware logs follow this format:
```
[YYYY-MM-DD HH:MM:SS] PREFIX [MODULE]: message
[+<milliseconds>ms] PREFIX [MODULE]: message
```

**Prefixes** (severity high to low): `ERR`, `WARN`, `INFO`, `DBG`

**Common Modules**:
- `SENSOR` - Sensor mode operations
- `IRRIG` - Irrigation mode operations
- `HubMode` - Hub mode operations
- `PMU`, `PMU_RX`, `PMU_REL` - Power management
- `AddressManager` - LoRa address management
- `HubRouter` - Hub routing operations
- `ReliableMessenger` - Reliable message delivery
- `CHT832X` - Temperature/humidity sensor
- `ValveCtrl` - Valve controller
- `LORA`, `NETWORK` - LoRa communication

Also parse Hub UART output patterns:
```
SENSOR_DATA <node_addr> <device_id> TEMP|HUM <value>
SENSOR_BATCH <node_addr> <device_id> <record_count>
Hub stats - Routed: X, Queued: Y, Dropped: Z
Unknown node 0xXXXX - sending reregister request
Registered nodes: N
```

### 3. Apply Filters (if specified)

If filters are in `$ARGUMENTS`:
- `--module <name>`: Keep only entries from that module
- `--level <level>`: Keep entries at or above level (ERR > WARN > INFO > DBG)
- `--node <addr>`: Keep entries mentioning that node address
- `--errors`: Equivalent to `--level WARN`

### 4. Analyze and Report

#### Always Include:

**a) Overview**
- Time range (first to last timestamp)
- Total log lines analyzed
- Error count, warning count

**b) Errors and Warnings** (grouped by type)
Focus on these categories:
- **Initialization failures**: `Failed to initialize`, sensor/flash init errors
- **Communication errors**: `Failed to send`, `timeout`, `CRC error`
- **Protocol issues**: `Unknown node`, `Invalid`, `NULL.*payload`
- **Power/PMU errors**: `Ready for sleep failed`, PMU communication issues

**c) Communication Analysis** (primary focus)
- Message statistics from `Hub stats - Routed: X, Queued: Y, Dropped: Z`
- Delivery rate calculation (routed / (routed + dropped))
- RSSI values from `RSSI=-XX dBm` patterns (signal quality)
- Node registration status from `Registered nodes:` lines
- Unregistered node attempts from `Unknown node 0xXXXX` patterns

**d) Node Health Summary** (if multi-node data present)
For each node detected:
- Node address and type (if determinable)
- Status: Active/Inactive (based on message recency)
- Message count
- Average RSSI (if available)
- Error count for that node

**e) Anomalies Detected**
- Sensor out of range: temperature < -40C or > 85C, humidity < 0% or > 100%
- Registration loops: repeated `reregister request` for same node
- Communication gaps: long periods without messages from a node
- Clock issues: `RTC not running`, timestamp discontinuities

#### If `--detailed` flag:
Additionally include:
- Full chronological list of errors/warnings
- All matching log entries
- Per-module breakdown of activity
- Sensor data statistics (min/max/avg readings if present)

### 5. Provide Recommendations

Based on findings, suggest specific actions:

| Issue | Recommendation |
|-------|----------------|
| Initialization errors | Check wiring, verify hardware connections |
| Communication timeouts | Check antenna connection, node positioning, reduce distance |
| Low RSSI (< -100 dBm) | Move nodes closer, check for interference, verify antenna |
| Unknown node errors | Node lost persisted address - may need re-registration |
| PMU errors | Check UART connection between RP2040 and PMU |
| CRC errors | Check for RF interference, verify LoRa settings match |
| Sensor out of range | Check sensor placement, verify calibration |
| High drop rate | Network congestion - consider staggering transmissions |

## Example Usage

```
/bramble-logs                              # Prompt for pasted logs, show summary
/bramble-logs ~/logs/hub.log               # Analyze file
/bramble-logs --errors                     # Show only errors/warnings from pasted logs
/bramble-logs --module HubMode --detailed  # Detailed hub analysis
/bramble-logs --node 0x0004                # Filter to specific node
```

## Error Patterns Quick Reference

| Pattern | Meaning |
|---------|---------|
| `Failed to initialize external flash` | SPI flash chip not responding |
| `Failed to initialize CHT832X sensor` | I2C temp/humidity sensor not found |
| `Unknown node 0xXXXX` | Message from unregistered node address |
| `Hub sync timeout` | Hub didn't respond to node heartbeat |
| `Ready for sleep failed` | PMU sleep command failed |
| `Failed to send` | LoRa radio transmission error |
| `CRC error` | Message corruption detected |
| `reregister request` | Hub asking unknown node to re-register |
