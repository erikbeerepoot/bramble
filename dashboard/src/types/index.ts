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
  address: number;
  name: string | null;
  location: string | null;
  notes: string | null;
  zone_id: number | null;
  updated_at: number | null;
}

export interface Node {
  address: number;
  device_id: number | null;
  type: string;
  online: boolean;
  last_seen_seconds: number;
  metadata?: NodeMetadata;
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
  node_address?: number;
  device_id?: number | null;
  temperature_raw?: number;
  humidity_raw?: number;
  flags?: number;
  received_at?: number;
  sample_count?: number;  // Present in downsampled responses
}

export interface SensorDataResponse {
  node_address: number;
  count: number;
  downsampled?: boolean;
  readings: SensorReading[];
}

export interface NodeStatistics {
  address: number;
  device_id: number | null;
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
