# Battery Life Analysis - Bramble Sensor Node

**Date:** 2026-01-30
**Firmware:** v0.2.16
**Hardware:** SENSOR variant

## Measured Values

| Parameter | Value | Method |
|-----------|-------|--------|
| Wake current | 60mA | 6mV over 0.1Ω shunt |
| Sleep current | ~µA | Not measurable on regular multimeter |

## Wake Cycle Timing

### Quiet Wake (no transmission) - ~1.5 seconds

| Phase | Duration |
|-------|----------|
| Boot → Sensor SM ready | 937ms |
| Sensor read + check backlog | ~500ms |
| **Total** | **~1.5s** |

### Transmit Wake (with radio) - ~5 seconds

| Phase | Duration | Notes |
|-------|----------|-------|
| Boot → Sensor SM ready | 937ms | PMU state restore takes 580ms |
| Sensor read + batch transmit | ~1s | |
| Waiting for heartbeat response | ~2s | Radio latency |
| Batch ACK + receive window | ~1s | 500ms window + processing |
| **Total** | **~5s** | |

## Duty Cycle

- Wake interval: 60 seconds
- Transmit interval: 300 seconds (5 minutes)
- Quiet wakes per cycle: 5
- Transmit wakes per cycle: 1

## Battery Life Calculation

**Battery:** 2 packs of 3×AAA in series
**Capacity:** ~1000mAh

### Per 5-minute cycle

| Activity | Time | Current | Energy |
|----------|------|---------|--------|
| 5 quiet wakes | 7.5s | 60mA | 0.125 mAh |
| 1 transmit wake | 5s | 60mA | 0.083 mAh |
| Sleep | 287s | ~0.01mA | 0.048 mAh |
| **Total** | 300s | | **~0.26 mAh** |

**Average current:** 3.1 mAh/hour

### Estimated Battery Life: ~13 days

## Optimization Opportunities

1. **Increase wake interval** - 60s → 120s or 300s could nearly double or 5x life
2. **Reduce transmit frequency** - batch less often than every 300s
3. **Shorten PMU state restore** - 580ms of 937ms init is waiting for state
4. **Add parallel battery pack** - doubles capacity to ~26 days
