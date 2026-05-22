---
name: bramble-hub-logs
description: Fetch Docker logs from the bramble hub via SSH and search for patterns.
---

# Bramble Hub Logs

Fetch logs from Docker containers running on the bramble hub (Raspberry Pi) via SSH, optionally filtering by a pattern.

## Arguments

`$ARGUMENTS` - Pattern to search for, plus optional flags
- `<pattern>` - Text or regex pattern to search for in the logs (required)
- `--container <name>` - Docker container name (default: search all containers)
- `--lines <N>` - Number of log lines to fetch (default: 200)
- `--since <duration>` - Only show logs since duration (e.g., `1h`, `30m`, `2h`) (default: 1h)

## Instructions

### 1. Parse Arguments

Extract from `$ARGUMENTS`:
- **pattern**: The search pattern (first positional argument, required)
- **container**: From `--container` flag, or empty for all containers
- **lines**: From `--lines` flag, default 200
- **since**: From `--since` flag, default "1h"

If no pattern is provided, ask the user what they want to search for.

### 2. SSH Connection Details

```
Host: bramble-hub.local
SSH Key: ~/.ssh/bramble_hub
User: root (required for Docker access)
```

### 3. Fetch Logs

If a specific container is given:
```bash
ssh -i ~/.ssh/bramble_hub -o ConnectTimeout=5 -o StrictHostKeyChecking=accept-new root@bramble-hub.local \
  "docker logs --tail <lines> --since <since> <container> 2>&1" 2>&1
```

If no container specified, first list running containers, then fetch logs from all:
```bash
# List containers
ssh -i ~/.ssh/bramble_hub -o ConnectTimeout=5 -o StrictHostKeyChecking=accept-new root@bramble-hub.local \
  "docker ps --format '{{.Names}}'" 2>&1

# Then for each container, fetch logs
ssh -i ~/.ssh/bramble_hub -o ConnectTimeout=5 -o StrictHostKeyChecking=accept-new root@bramble-hub.local \
  "docker logs --tail <lines> --since <since> <container_name> 2>&1" 2>&1
```

### 4. Filter by Pattern

After fetching logs, use Grep or manual filtering to find lines matching the pattern. Include 2 lines of context before and after each match for readability.

### 5. Present Results

Report:
- Which container(s) were searched
- Number of matching lines found
- The matching lines with context, grouped by container
- A brief summary of what the matches suggest

If no matches found, report that and suggest:
- Trying a broader pattern
- Increasing `--since` duration
- Checking a different container

### 6. Analyze Matches

After showing the matches, provide a brief analysis:
- Are there error patterns?
- Is there a trend (increasing frequency, etc.)?
- Any actionable recommendations?

## Example Usage

```
/bramble-hub-logs LIST_NODES                     # Search for LIST_NODES in all containers, last 1h
/bramble-hub-logs timeout --since 2h             # Search for "timeout" in last 2 hours
/bramble-hub-logs "error" --container bramble-api # Search specific container
/bramble-hub-logs SENSOR_DATA --lines 500        # Get more lines
```

## Error Handling

| Error | Action |
|-------|--------|
| SSH connection refused | Check that the Pi is online and SSH is running |
| Permission denied | Verify SSH key at ~/.ssh/bramble_hub exists and has correct permissions |
| Host not found | Check that bramble-hub.local is resolvable (mDNS) or use IP address |
| No containers running | Report that Docker has no running containers |
| Docker not found | Report that Docker is not installed or not in PATH |
