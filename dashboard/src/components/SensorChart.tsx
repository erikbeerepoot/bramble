import Plot from 'react-plotly.js';
import type { SensorReading } from '../types';

// Maximum gap (in seconds) between points before breaking the line
const GAP_THRESHOLD_SECONDS = 10 * 60; // 10 minutes

interface DataSegment {
  timestamps: Date[];
  values: number[];
}

/**
 * Split readings into separate segments where time gaps exceed the threshold.
 * Each segment becomes a separate trace so fills don't span across gaps.
 */
function splitIntoSegments(
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

interface SensorChartProps {
  readings: SensorReading[];
  dataKey: 'temperature_celsius' | 'humidity_percent';
  title: string;
  yAxisLabel: string;
  color: string;
  startTime?: number; // Unix timestamp
  endTime?: number; // Unix timestamp
}

function SensorChart({
  readings,
  dataKey,
  title,
  yAxisLabel,
  color,
  startTime,
  endTime,
}: SensorChartProps) {
  // Sort readings by timestamp (oldest first for proper line chart)
  const sortedReadings = [...readings].sort((a, b) => a.timestamp - b.timestamp);

  // Split into segments at large gaps so fills don't span across gaps
  const segments = splitIntoSegments(sortedReadings, dataKey, GAP_THRESHOLD_SECONDS);

  // Calculate statistics from original readings
  const validValues = sortedReadings
    .map((r) => r[dataKey])
    .filter((v) => v !== null && v !== undefined) as number[];
  const min = validValues.length > 0 ? Math.min(...validValues) : 0;
  const max = validValues.length > 0 ? Math.max(...validValues) : 0;
  const avg =
    validValues.length > 0 ? validValues.reduce((a, b) => a + b, 0) / validValues.length : 0;

  // Calculate y-axis range with 10% padding
  const yPadding = (max - min) * 0.1 || 1; // fallback to 1 if min===max
  const yMin = min - yPadding;
  const yMax = max + yPadding;

  return (
    <div>
      <div className="flex flex-wrap items-center justify-between gap-1 mb-2">
        <h4 className="font-medium text-gray-700">{title}</h4>
        <div className="flex items-center gap-3 text-sm text-gray-500">
          <span>Min: {min.toFixed(1)}</span>
          <span>Avg: {avg.toFixed(1)}</span>
          <span>Max: {max.toFixed(1)}</span>
        </div>
      </div>
      <Plot
        data={segments.map((segment, index) => ({
          x: segment.timestamps,
          y: segment.values,
          type: 'scatter' as const,
          mode: 'lines' as const,
          name: title,
          showlegend: index === 0, // Only show legend for first segment
          line: {
            color: color,
            width: 2,
          },
          fill: 'tozeroy' as const,
          fillcolor: `${color}20`,
        }))}
        layout={{
          autosize: true,
          height: 250,
          margin: { l: 50, r: 20, t: 10, b: 40 },
          xaxis: {
            type: 'date',
            tickformat: '%H:%M\n%b %d',
            gridcolor: '#f3f4f6',
            range:
              startTime && endTime
                ? [new Date(startTime * 1000), new Date(endTime * 1000)]
                : undefined,
          },
          yaxis: {
            title: { text: yAxisLabel },
            gridcolor: '#f3f4f6',
            range: validValues.length > 0 ? [yMin, yMax] : undefined,
          },
          paper_bgcolor: 'transparent',
          plot_bgcolor: 'transparent',
          hovermode: 'x unified',
          showlegend: false,
          shapes: [
            ...(validValues.length > 0
              ? [
                  {
                    type: 'line' as const,
                    x0: startTime
                      ? new Date(startTime * 1000)
                      : new Date(sortedReadings[0].timestamp * 1000),
                    x1: endTime
                      ? new Date(endTime * 1000)
                      : new Date(sortedReadings[sortedReadings.length - 1].timestamp * 1000),
                    y0: avg,
                    y1: avg,
                    line: {
                      color: color,
                      width: 1.5,
                      dash: 'dash' as const,
                    },
                  },
                ]
              : []),
          ],
        }}
        config={{
          responsive: true,
          displayModeBar: false,
        }}
        style={{ width: '100%' }}
      />
    </div>
  );
}

export default SensorChart;
