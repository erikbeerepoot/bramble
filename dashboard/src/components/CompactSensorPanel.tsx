import type { SensorReading, NodeStatistics } from '../types';
import Sparkline from './Sparkline';

type SensorDataKey = 'temperature_celsius' | 'humidity_percent';

type Trend = 'Steady' | 'Increasing' | 'Decreasing' | 'Fluctuating';

function computeTrend(readings: SensorReading[], dataKey: SensorDataKey): Trend | null {
  if (readings.length < 3) return null;
  const sorted = [...readings].sort((a, b) => a.timestamp - b.timestamp);
  const values = sorted.map((r) => r[dataKey]);
  const min = Math.min(...values);
  const max = Math.max(...values);
  const range = max - min;
  const mean = values.reduce((a, b) => a + b, 0) / values.length;

  // "Steady" threshold — small range relative to mean
  // For temp/humidity, ~0.5 absolute or ~1% relative counts as flat
  const isSmallRange = range < 0.5 || (mean !== 0 && range / Math.abs(mean) < 0.01);
  if (isSmallRange) return 'Steady';

  // Linear fit: slope via least squares
  const n = values.length;
  const xs = values.map((_, i) => i);
  const xMean = (n - 1) / 2;
  let num = 0;
  let den = 0;
  for (let i = 0; i < n; i++) {
    num += (xs[i] - xMean) * (values[i] - mean);
    den += (xs[i] - xMean) ** 2;
  }
  const slope = den === 0 ? 0 : num / den;
  const netChange = slope * (n - 1);

  // If the net linear change accounts for >50% of the observed range, it's trending
  if (Math.abs(netChange) > range * 0.5) {
    return netChange > 0 ? 'Increasing' : 'Decreasing';
  }
  return 'Fluctuating';
}

interface CompactSensorCardProps {
  label: string;
  unit: string;
  dataKey: SensorDataKey;
  readings: SensorReading[];
  color: string;
  bgClassName: string;
  borderClassName: string;
  decimals?: number;
  startTime?: number;
  endTime?: number;
}

function CompactSensorCard({
  label,
  unit,
  dataKey,
  readings,
  color,
  bgClassName,
  borderClassName,
  decimals = 1,
  startTime,
  endTime,
}: CompactSensorCardProps) {
  const sorted = [...readings].sort((a, b) => a.timestamp - b.timestamp);
  const latest = sorted[sorted.length - 1]?.[dataKey];
  const trend = computeTrend(readings, dataKey);

  return (
    <div
      className={`p-4 rounded-xl border ${bgClassName} ${borderClassName} transition-shadow duration-150 hover:shadow-md`}
    >
      <div className="flex items-start justify-between mb-2">
        <div>
          <div className="text-xs text-gray-600 mb-0.5">{label}</div>
          <div className="flex items-baseline gap-1">
            <span className="text-2xl font-semibold text-gray-900">
              {typeof latest === 'number' ? latest.toFixed(decimals) : '-'}
            </span>
            <span className="text-base text-gray-500">{unit}</span>
          </div>
        </div>
        {trend && (
          <span className="text-xs text-gray-500 mt-0.5" style={{ color }}>
            {trend}
          </span>
        )}
      </div>
      <div className="h-20 -mx-1">
        <Sparkline
          readings={readings}
          dataKey={dataKey}
          color={color}
          height={80}
          interactive
          unit={unit}
          startTime={startTime}
          endTime={endTime}
        />
      </div>
    </div>
  );
}

interface CompactSensorPanelProps {
  readings: SensorReading[];
  // Kept for API compatibility / future use (e.g. stats-based labels)
  statistics?: NodeStatistics | null;
  startTime?: number;
  endTime?: number;
}

function CompactSensorPanel({ readings, startTime, endTime }: CompactSensorPanelProps) {
  if (readings.length === 0) return null;

  return (
    <div className="grid grid-cols-1 md:grid-cols-2 gap-4 mb-4">
      <CompactSensorCard
        label="Temperature"
        unit="°C"
        dataKey="temperature_celsius"
        readings={readings}
        color="#f97316"
        bgClassName="bg-gradient-to-br from-orange-50 to-white"
        borderClassName="border-orange-100"
        startTime={startTime}
        endTime={endTime}
      />
      <CompactSensorCard
        label="Humidity"
        unit="%"
        dataKey="humidity_percent"
        readings={readings}
        color="#3b82f6"
        bgClassName="bg-gradient-to-br from-blue-50 to-white"
        borderClassName="border-blue-100"
        startTime={startTime}
        endTime={endTime}
      />
    </div>
  );
}

export default CompactSensorPanel;
