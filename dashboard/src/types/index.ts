export interface Zone {
  id: number;
  name: string;
  color: string;
  description: string | null;
}

export interface ZonesResponse {
  count: number;
  zones: Zone[];
}

export interface NodeMetadata {
  device_id: number;
  name: string | null;
  notes: string | null;
  zone_id: number | null;
  updated_at: number | null;
}

export interface NodeStatus {
  device_id: number;
  address: number | null;
  battery_level: number | null;
  error_flags: number | null;
  signal_strength: number | null;
  uptime_seconds: number | null;
  pending_records: number | null;
  updated_at: number | null;
}

// Node type constants (match firmware/API values)
export const NodeType = {
  SENSOR: 'SENSOR',
  IRRIGATION: 'IRRIGATION',
  CONTROLLER: 'CONTROLLER',
  HUB: 'HUB',
  GENERIC: 'GENERIC',
} as const;

export type NodeTypeValue = typeof NodeType[keyof typeof NodeType];

export interface Node {
  device_id: number;  // Primary identifier
  address: number;    // LoRa address (for routing)
  type: NodeTypeValue;
  online: boolean;
  last_seen_seconds: number;
  firmware_version: string | null;
  metadata?: NodeMetadata;
  status?: NodeStatus;
  hub_queue_count?: number | null;
}

// Error flag constants (match firmware ERR_FLAG_* in message.h)
export const ERR_FLAG_NONE              = 0x00;
export const ERR_FLAG_SENSOR_FAILURE    = 0x01;
export const ERR_FLAG_FLASH_FAILURE     = 0x02;
export const ERR_FLAG_FLASH_FULL        = 0x04;
export const ERR_FLAG_PMU_FAILURE       = 0x08;
export const ERR_FLAG_BATTERY_LOW       = 0x10;
export const ERR_FLAG_BATTERY_CRITICAL  = 0x20;
export const ERR_FLAG_RTC_NOT_SYNCED    = 0x40;
export const ERR_FLAG_RADIO_ISSUE       = 0x80;

// Error flag descriptions for display
export const ERROR_FLAG_INFO: Record<number, { label: string; severity: 'warning' | 'error' }> = {
  [ERR_FLAG_SENSOR_FAILURE]:   { label: 'Sensor Failure',   severity: 'error' },
  [ERR_FLAG_FLASH_FAILURE]:    { label: 'Flash Failure',    severity: 'error' },
  [ERR_FLAG_FLASH_FULL]:       { label: 'Flash Full',       severity: 'warning' },
  [ERR_FLAG_PMU_FAILURE]:      { label: 'PMU Failure',      severity: 'error' },
  [ERR_FLAG_BATTERY_LOW]:      { label: 'Low Battery',      severity: 'warning' },
  [ERR_FLAG_BATTERY_CRITICAL]: { label: 'Critical Battery', severity: 'error' },
  [ERR_FLAG_RTC_NOT_SYNCED]:   { label: 'RTC Not Synced',   severity: 'warning' },
  [ERR_FLAG_RADIO_ISSUE]:      { label: 'Radio Issue',      severity: 'warning' },
};

/**
 * Parse error flags into an array of active errors
 */
export function parseErrorFlags(flags: number | null): Array<{ flag: number; label: string; severity: 'warning' | 'error' }> {
  if (flags === null || flags === 0) return [];

  const errors: Array<{ flag: number; label: string; severity: 'warning' | 'error' }> = [];

  for (const [flagStr, info] of Object.entries(ERROR_FLAG_INFO)) {
    const flag = parseInt(flagStr, 10);
    if (flags & flag) {
      errors.push({ flag, ...info });
    }
  }

  return errors;
}

/**
 * Get the highest severity from error flags
 */
export function getHealthStatus(flags: number | null): 'healthy' | 'warning' | 'error' {
  if (flags === null || flags === 0) return 'healthy';

  const errors = parseErrorFlags(flags);
  if (errors.some(e => e.severity === 'error')) return 'error';
  if (errors.some(e => e.severity === 'warning')) return 'warning';
  return 'healthy';
}

/**
 * Signal quality thresholds based on LoRa research
 *
 * RSSI thresholds (dBm):
 * - Excellent: > -65 dBm (strong signal)
 * - Good: > -85 dBm (suitable for most applications)
 * - Fair: > -100 dBm (may work but less reliable)
 * - Poor: <= -100 dBm (near receiver sensitivity limit)
 *
 * SNR thresholds (dB):
 * - When SNR >= 7 dB: use RSSI as quality indicator
 * - When SNR < 7 dB: use SNR as quality indicator (more meaningful near noise floor)
 */
const RSSI_EXCELLENT = -65;
const RSSI_GOOD = -85;
const RSSI_FAIR = -100;

const SNR_USE_RSSI_THRESHOLD = 7;  // Above this, RSSI is more meaningful
const SNR_GOOD = 0;
const SNR_FAIR = -10;

/**
 * Get signal quality label from RSSI and optionally SNR
 *
 * When SNR is provided and < 7 dB, it's used as the primary quality indicator
 * since it becomes more meaningful near the noise floor.
 */
export function getSignalQuality(
  rssi: number | null,
  snr?: number | null
): { label: string; bars: number; color: string } {
  if (rssi === null) return { label: 'Unknown', bars: 0, color: 'gray' };

  // RSSI is typically negative (dBm)
  // Convert to absolute if positive (some systems report absolute)
  const absRssi = rssi > 0 ? -rssi : rssi;

  // When SNR is available and low, use it as the quality indicator
  if (snr !== null && snr !== undefined && snr < SNR_USE_RSSI_THRESHOLD) {
    if (snr >= SNR_GOOD) return { label: 'Good', bars: 3, color: 'green' };
    if (snr >= SNR_FAIR) return { label: 'Fair', bars: 2, color: 'yellow' };
    return { label: 'Poor', bars: 1, color: 'red' };
  }

  // Use RSSI when SNR is good or unavailable
  if (absRssi > RSSI_EXCELLENT) return { label: 'Excellent', bars: 4, color: 'green' };
  if (absRssi > RSSI_GOOD) return { label: 'Good', bars: 3, color: 'green' };
  if (absRssi > RSSI_FAIR) return { label: 'Fair', bars: 2, color: 'yellow' };
  return { label: 'Poor', bars: 1, color: 'red' };
}

/**
 * Get battery status from level
 */
export function getBatteryStatus(level: number | null): { label: string; color: string; isExternal: boolean } {
  if (level === null) return { label: 'Unknown', color: 'gray', isExternal: false };
  if (level === 255) return { label: 'External', color: 'blue', isExternal: true };
  if (level > 60) return { label: `${level}%`, color: 'green', isExternal: false };
  if (level > 20) return { label: `${level}%`, color: 'yellow', isExternal: false };
  return { label: `${level}%`, color: 'red', isExternal: false };
}

/**
 * Format uptime duration
 */
export function formatUptime(seconds: number | null): string {
  if (seconds === null) return 'Unknown';

  const days = Math.floor(seconds / 86400);
  const hours = Math.floor((seconds % 86400) / 3600);
  const minutes = Math.floor((seconds % 3600) / 60);

  if (days > 0) return `${days}d ${hours}h ${minutes}m`;
  if (hours > 0) return `${hours}h ${minutes}m`;
  return `${minutes}m`;
}

export interface NodesResponse {
  count: number;
  nodes: Node[];
}

export interface SensorReading {
  timestamp: number;
  temperature_celsius: number;
  humidity_percent: number;
  // Optional fields (not included in compact/downsampled responses)
  device_id?: number;
  address?: number | null;
  temperature_raw?: number;
  humidity_raw?: number;
  flags?: number;
  received_at?: number;
  sample_count?: number;  // Present in downsampled responses
}

export interface SensorDataResponse {
  device_id: number;
  count: number;
  downsampled?: boolean;
  readings: SensorReading[];
}

export interface NodeStatistics {
  device_id: number;
  address: number | null;
  node_type: string;
  first_seen_at: number;
  last_seen_at: number;
  total_readings: number;
  reading_count?: number;  // Count of readings in the queried time range
  temperature: {
    min_celsius: number | null;
    max_celsius: number | null;
    avg_celsius: number | null;
  };
  humidity: {
    min_percent: number | null;
    max_percent: number | null;
    avg_percent: number | null;
  };
}

export type TimeRange = '1h' | '6h' | '24h' | '7d' | '30d' | 'custom';

export interface TimeRangeConfig {
  label: string;
  seconds: number;
}

export interface CustomTimeRange {
  startTime: number;
  endTime: number;
}

export const TIME_RANGES: Record<Exclude<TimeRange, 'custom'>, TimeRangeConfig> = {
  '1h': { label: 'Last Hour', seconds: 3600 },
  '6h': { label: 'Last 6 Hours', seconds: 21600 },
  '24h': { label: 'Last 24 Hours', seconds: 86400 },
  '7d': { label: 'Last 7 Days', seconds: 604800 },
  '30d': { label: 'Last 30 Days', seconds: 2592000 },
};

/**
 * Get backlog status color based on count
 */
export function getBacklogStatus(count: number | null | undefined): { color: string; label: string } {
  if (count === null || count === undefined) return { color: 'gray', label: 'Unknown' };
  if (count === 0) return { color: 'green', label: 'Clear' };
  if (count < 50) return { color: 'yellow', label: 'Pending' };
  return { color: 'red', label: 'Backlogged' };
}

/**
 * Overall node health for dashboard display.
 * Returns a single color representing the worst condition across
 * error flags, battery, and signal strength.
 *
 * red    = error-level issue (sensor/flash/PMU failure, critical battery, offline)
 * orange = degraded (poor signal, high backlog)
 * yellow = warning (low battery, flash full, RTC not synced, radio issue)
 * green  = healthy
 * gray   = unknown (no status data)
 */
export type OverallHealth = 'red' | 'orange' | 'yellow' | 'green' | 'gray';

export function getOverallNodeHealth(node: Node): OverallHealth {
  if (!node.online) return 'red';
  if (!node.status) return 'gray';

  const { error_flags, battery_level, signal_strength, pending_records } = node.status;

  // Check error flags for error-level issues
  const health = getHealthStatus(error_flags);
  if (health === 'error') return 'red';

  // Poor signal is a degraded (orange) condition
  if (signal_strength !== null) {
    const absRssi = signal_strength > 0 ? -signal_strength : signal_strength;
    if (absRssi <= RSSI_FAIR) return 'orange';
  }

  // High backlog is degraded
  if (pending_records !== null && pending_records >= 50) return 'orange';

  // Warning-level error flags
  if (health === 'warning') return 'yellow';

  // Low battery (not critical, that's caught by error flags) is a warning
  if (battery_level !== null && battery_level !== 255 && battery_level <= 20) return 'yellow';

  return 'green';
}
