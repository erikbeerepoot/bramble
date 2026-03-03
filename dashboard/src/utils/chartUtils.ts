import type { SensorReading } from '../types';

// Maximum gap (in seconds) between points before breaking the line
export const GAP_THRESHOLD_SECONDS = 10 * 60; // 10 minutes

export interface DataSegment {
  timestamps: Date[];
  values: number[];
}

/**
 * Split readings into separate segments where time gaps exceed the threshold.
 * Each segment becomes a separate trace so fills/lines don't span across gaps.
 */
export function splitIntoSegments(
  sortedReadings: SensorReading[],
  dataKey: 'temperature_celsius' | 'humidity_percent',
  gapThreshold: number
): DataSegment[] {
  if (sortedReadings.length === 0) {
    return [];
  }

  const segments: DataSegment[] = [];
  let currentSegment: DataSegment = { timestamps: [], values: [] };

  for (let i = 0; i < sortedReadings.length; i++) {
    const reading = sortedReadings[i];

    // Check for gap from previous reading
    if (i > 0) {
      const gap = reading.timestamp - sortedReadings[i - 1].timestamp;
      if (gap > gapThreshold && currentSegment.timestamps.length > 0) {
        // Save current segment and start a new one
        segments.push(currentSegment);
        currentSegment = { timestamps: [], values: [] };
      }
    }

    currentSegment.timestamps.push(new Date(reading.timestamp * 1000));
    currentSegment.values.push(reading[dataKey]);
  }

  // Don't forget the last segment
  if (currentSegment.timestamps.length > 0) {
    segments.push(currentSegment);
  }

  return segments;
}

// 10 distinct colors for multi-sensor comparison traces
export const COMPARISON_COLORS = [
  '#2563eb', // blue
  '#dc2626', // red
  '#16a34a', // green
  '#9333ea', // purple
  '#ea580c', // orange
  '#0891b2', // cyan
  '#be185d', // pink
  '#854d0e', // amber
  '#4f46e5', // indigo
  '#059669', // emerald
];
