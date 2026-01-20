import Plot from 'react-plotly.js';
import type { SensorReading } from '../types';

interface SensorChartProps {
  readings: SensorReading[];
  dataKey: 'temperature_celsius' | 'humidity_percent';
  title: string;
  yAxisLabel: string;
  color: string;
  startTime?: number;  // Unix timestamp
  endTime?: number;    // Unix timestamp
}

function SensorChart({ readings, dataKey, title, yAxisLabel, color, startTime, endTime }: SensorChartProps) {
  // Sort readings by timestamp (oldest first for proper line chart)
  const sortedReadings = [...readings].sort((a, b) => a.timestamp - b.timestamp);

  const timestamps = sortedReadings.map(r => new Date(r.timestamp * 1000));
  const values = sortedReadings.map(r => r[dataKey]);

  // Calculate statistics for the current data
  const validValues = values.filter(v => v !== null && v !== undefined) as number[];
  const min = validValues.length > 0 ? Math.min(...validValues) : 0;
  const max = validValues.length > 0 ? Math.max(...validValues) : 0;
  const avg = validValues.length > 0 ? validValues.reduce((a, b) => a + b, 0) / validValues.length : 0;

  return (
    <div>
      <div className="flex items-center justify-between mb-2">
        <h4 className="font-medium text-gray-700">{title}</h4>
        <div className="flex items-center space-x-4 text-sm text-gray-500">
          <span>Min: {min.toFixed(1)}</span>
          <span>Avg: {avg.toFixed(1)}</span>
          <span>Max: {max.toFixed(1)}</span>
        </div>
      </div>
      <Plot
        data={[
          {
            x: timestamps,
            y: values,
            type: 'scatter',
            mode: 'lines',
            name: title,
            line: {
              color: color,
              width: 2,
            },
            fill: 'tozeroy',
            fillcolor: `${color}20`,
          },
        ]}
        layout={{
          autosize: true,
          height: 250,
          margin: { l: 50, r: 20, t: 10, b: 40 },
          xaxis: {
            type: 'date',
            tickformat: '%H:%M\n%b %d',
            gridcolor: '#f3f4f6',
            range: startTime && endTime
              ? [new Date(startTime * 1000), new Date(endTime * 1000)]
              : undefined,
          },
          yaxis: {
            title: { text: yAxisLabel },
            gridcolor: '#f3f4f6',
          },
          paper_bgcolor: 'transparent',
          plot_bgcolor: 'transparent',
          hovermode: 'x unified',
          showlegend: false,
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
