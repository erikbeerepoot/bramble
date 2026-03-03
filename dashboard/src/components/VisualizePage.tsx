import { useState, useEffect, useRef } from 'react';
import type { SensorReading, TimeRange, CustomTimeRange } from '../types';
import { TIME_RANGES } from '../types';
import { useAppContext } from '../App';
import { getNodeSensorData } from '../api/client';
import { COMPARISON_COLORS } from '../utils/chartUtils';
import TimeRangeSelector from './TimeRangeSelector';
import SensorPicker from './SensorPicker';
import ComparisonChart from './ComparisonChart';

type Metric = 'temperature' | 'humidity';

function VisualizePage() {
  const { nodes } = useAppContext();
  const [selectedDeviceIds, setSelectedDeviceIds] = useState<string[]>([]);
  const [metric, setMetric] = useState<Metric>('temperature');
  const [timeRange, setTimeRange] = useState<TimeRange>('24h');
  const [customRange, setCustomRange] = useState<CustomTimeRange>(() => {
    const now = Math.floor(Date.now() / 1000);
    return { startTime: now - 86400, endTime: now };
  });
  const [sensorData, setSensorData] = useState<Map<string, SensorReading[]>>(new Map());
  const [loading, setLoading] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const abortControllerRef = useRef<AbortController | null>(null);

  useEffect(() => {
    if (selectedDeviceIds.length === 0) {
      setSensorData(new Map());
      return;
    }

    // Abort previous requests
    if (abortControllerRef.current) {
      abortControllerRef.current.abort();
    }
    const controller = new AbortController();
    abortControllerRef.current = controller;

    let startTime: number | undefined;
    let endTime: number | undefined;
    const now = Math.floor(Date.now() / 1000);

    if (timeRange === 'custom') {
      startTime = customRange.startTime;
      endTime = customRange.endTime;
    } else {
      startTime = now - TIME_RANGES[timeRange].seconds;
      endTime = now;
    }

    setLoading(true);
    setError(null);

    const fetches = selectedDeviceIds.map((deviceId) =>
      getNodeSensorData(deviceId, {
        startTime,
        endTime,
        downsample: 500,
        signal: controller.signal,
      }).then((response) => ({ deviceId, readings: response.readings }))
    );

    Promise.allSettled(fetches).then((results) => {
      if (controller.signal.aborted) return;

      const newData = new Map<string, SensorReading[]>();
      let hasError = false;

      for (const result of results) {
        if (result.status === 'fulfilled') {
          newData.set(result.value.deviceId, result.value.readings);
        } else {
          hasError = true;
        }
      }

      setSensorData(newData);
      setLoading(false);
      if (hasError && newData.size === 0) {
        setError('Failed to load sensor data.');
      }
    });

    return () => controller.abort();
  }, [selectedDeviceIds, timeRange, customRange]);

  // Compute time bounds for chart x-axis
  const now = Math.floor(Date.now() / 1000);
  let chartStartTime: number | undefined;
  let chartEndTime: number | undefined;
  if (timeRange === 'custom') {
    chartStartTime = customRange.startTime;
    chartEndTime = customRange.endTime;
  } else {
    chartStartTime = now - TIME_RANGES[timeRange].seconds;
    chartEndTime = now;
  }

  const dataKey = metric === 'temperature' ? 'temperature_celsius' : 'humidity_percent';
  const yAxisLabel = metric === 'temperature' ? 'Temperature (\u00B0C)' : 'Humidity (%)';

  // Build traces for selected sensors that have data
  const traces = selectedDeviceIds
    .map((deviceId, index) => {
      const readings = sensorData.get(deviceId);
      if (!readings) return null;
      const node = nodes.find((n) => n.device_id === deviceId);
      const label = node?.metadata?.name || deviceId;
      const color = COMPARISON_COLORS[index % COMPARISON_COLORS.length];
      return { deviceId, label, color, readings };
    })
    .filter((t): t is NonNullable<typeof t> => t !== null);

  return (
    <div className="space-y-4">
      <h2 className="text-xl font-bold text-gray-800">Visualize</h2>

      <div className="card space-y-4">
        {/* Controls row */}
        <div className="flex flex-col sm:flex-row sm:items-start gap-4">
          {/* Metric toggle */}
          <div className="flex gap-1">
            <button
              onClick={() => setMetric('temperature')}
              className={`px-3 py-1 text-sm rounded-md transition-colors ${
                metric === 'temperature'
                  ? 'bg-bramble-600 text-white'
                  : 'bg-gray-100 text-gray-600 hover:bg-gray-200'
              }`}
            >
              Temperature
            </button>
            <button
              onClick={() => setMetric('humidity')}
              className={`px-3 py-1 text-sm rounded-md transition-colors ${
                metric === 'humidity'
                  ? 'bg-bramble-600 text-white'
                  : 'bg-gray-100 text-gray-600 hover:bg-gray-200'
              }`}
            >
              Humidity
            </button>
          </div>

          <TimeRangeSelector
            value={timeRange}
            onChange={setTimeRange}
            customRange={customRange}
            onCustomRangeChange={setCustomRange}
          />
        </div>

        {/* Sensor picker */}
        <SensorPicker
          nodes={nodes}
          selectedIds={selectedDeviceIds}
          onChange={setSelectedDeviceIds}
          colors={COMPARISON_COLORS}
        />

        {/* Chart area */}
        {loading && (
          <div className="flex items-center justify-center py-12">
            <div className="inline-block animate-spin rounded-full h-8 w-8 border-4 border-bramble-600 border-t-transparent"></div>
          </div>
        )}

        {error && <p className="text-sm text-red-600">{error}</p>}

        {!loading && selectedDeviceIds.length === 0 && (
          <div className="text-center py-12 text-gray-500">
            <p className="text-lg">Select sensors above to compare their data</p>
          </div>
        )}

        {!loading && traces.length > 0 && (
          <ComparisonChart
            traces={traces}
            dataKey={dataKey}
            yAxisLabel={yAxisLabel}
            startTime={chartStartTime}
            endTime={chartEndTime}
          />
        )}

        {!loading && selectedDeviceIds.length > 0 && traces.length === 0 && !error && (
          <div className="text-center py-12 text-gray-500">
            <p>No data available for the selected sensors and time range.</p>
          </div>
        )}
      </div>
    </div>
  );
}

export default VisualizePage;
