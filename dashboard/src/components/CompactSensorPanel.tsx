import { TrendingUp, TrendingDown } from 'lucide-react';
import type { SensorReading, NodeStatistics } from '../types';
import Sparkline from './Sparkline';

type SensorDataKey = 'temperature_celsius' | 'humidity_percent';

interface CompactSensorCardProps {
  label: string;
  unit: string;
  dataKey: SensorDataKey;
  readings: SensorReading[];
  min: number | null;
  max: number | null;
  color: string;
  bgClassName: string;
  borderClassName: string;
  decimals?: number;
}

function CompactSensorCard({
  label,
  unit,
  dataKey,
  readings,
  min,
  max,
  color,
  bgClassName,
  borderClassName,
  decimals = 1,
}: CompactSensorCardProps) {
  const sorted = [...readings].sort((a, b) => a.timestamp - b.timestamp);
  const latest = sorted[sorted.length - 1]?.[dataKey];
  const first = sorted[0]?.[dataKey];
  const hasTrend = sorted.length >= 2 && typeof latest === 'number' && typeof first === 'number';
  const delta = hasTrend ? latest - first : 0;
  const trendUp = delta >= 0;

  return (
    <div className={`p-4 rounded-xl border ${bgClassName} ${borderClassName}`}>
      <div className="flex items-start justify-between mb-2">
        <div>
          <div className="text-xs text-gray-600 mb-0.5">{label}</div>
          <div className="flex items-baseline gap-2">
            <span className="text-2xl font-semibold text-gray-900">
              {typeof latest === 'number' ? latest.toFixed(decimals) : '-'}
              <span className="text-base font-normal text-gray-500 ml-0.5">{unit}</span>
            </span>
            {hasTrend && (
              <div
                className={`flex items-center gap-0.5 ${
                  trendUp ? 'text-green-600' : 'text-red-600'
                }`}
              >
                {trendUp ? (
                  <TrendingUp className="w-3 h-3" />
                ) : (
                  <TrendingDown className="w-3 h-3" />
                )}
                <span className="text-xs font-medium">
                  {trendUp ? '+' : ''}
                  {delta.toFixed(decimals)}
                  {unit === '°C' ? '°' : unit}
                </span>
              </div>
            )}
          </div>
        </div>
        <div className="text-right text-xs text-gray-500 shrink-0">
          <div>Min: {min !== null ? min.toFixed(decimals) : '-'}</div>
          <div>Max: {max !== null ? max.toFixed(decimals) : '-'}</div>
        </div>
      </div>
      <div className="h-16 -mx-1">
        <Sparkline readings={readings} dataKey={dataKey} color={color} height={64} />
      </div>
    </div>
  );
}

interface CompactSensorPanelProps {
  readings: SensorReading[];
  statistics: NodeStatistics | null;
}

function CompactSensorPanel({ readings, statistics }: CompactSensorPanelProps) {
  if (readings.length === 0) return null;

  return (
    <div className="grid grid-cols-1 md:grid-cols-2 gap-4 mb-4">
      <CompactSensorCard
        label="Temperature"
        unit="°C"
        dataKey="temperature_celsius"
        readings={readings}
        min={statistics?.temperature.min_celsius ?? null}
        max={statistics?.temperature.max_celsius ?? null}
        color="#f97316"
        bgClassName="bg-gradient-to-br from-orange-50 to-white"
        borderClassName="border-orange-100"
      />
      <CompactSensorCard
        label="Humidity"
        unit="%"
        dataKey="humidity_percent"
        readings={readings}
        min={statistics?.humidity.min_percent ?? null}
        max={statistics?.humidity.max_percent ?? null}
        color="#3b82f6"
        bgClassName="bg-gradient-to-br from-blue-50 to-white"
        borderClassName="border-blue-100"
      />
    </div>
  );
}

export default CompactSensorPanel;
