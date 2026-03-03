import Plot from 'react-plotly.js';
import type { SensorReading } from '../types';
import { GAP_THRESHOLD_SECONDS, splitIntoSegments } from '../utils/chartUtils';

interface TraceInput {
  deviceId: string;
  label: string;
  color: string;
  readings: SensorReading[];
}

interface ComparisonChartProps {
  traces: TraceInput[];
  dataKey: 'temperature_celsius' | 'humidity_percent';
  yAxisLabel: string;
  startTime?: number;
  endTime?: number;
}

function ComparisonChart({
  traces,
  dataKey,
  yAxisLabel,
  startTime,
  endTime,
}: ComparisonChartProps) {
  // Build Plotly traces: each input trace may produce multiple segments
  const plotlyTraces = traces.flatMap((trace) => {
    const sorted = [...trace.readings].sort((a, b) => a.timestamp - b.timestamp);
    const segments = splitIntoSegments(sorted, dataKey, GAP_THRESHOLD_SECONDS);

    return segments.map((segment, segmentIndex) => ({
      x: segment.timestamps,
      y: segment.values,
      type: 'scatter' as const,
      mode: 'lines' as const,
      name: trace.label,
      showlegend: segmentIndex === 0,
      legendgroup: trace.deviceId,
      line: {
        color: trace.color,
        width: 2,
      },
    }));
  });

  // Calculate y-axis range across all traces with 10% padding
  const allValues = traces.flatMap((t) => t.readings.map((r) => r[dataKey]));
  const min = allValues.length > 0 ? Math.min(...allValues) : 0;
  const max = allValues.length > 0 ? Math.max(...allValues) : 0;
  const yPadding = (max - min) * 0.1 || 1;
  const yMin = min - yPadding;
  const yMax = max + yPadding;

  return (
    <Plot
      data={plotlyTraces}
      layout={{
        autosize: true,
        height: 400,
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
          range: allValues.length > 0 ? [yMin, yMax] : undefined,
        },
        paper_bgcolor: 'transparent',
        plot_bgcolor: 'transparent',
        hovermode: 'x unified',
        showlegend: true,
        legend: {
          orientation: 'h',
          y: -0.2,
        },
      }}
      config={{
        responsive: true,
        displayModeBar: false,
      }}
      style={{ width: '100%' }}
    />
  );
}

export default ComparisonChart;
