import { useState, useEffect, useCallback } from 'react';
import type { Node, NodeStatistics, SensorReading, TimeRange, NodeMetadata, CustomTimeRange, Zone } from '../types';
import { TIME_RANGES, parseErrorFlags, formatUptime, getSignalQuality, getBatteryStatus } from '../types';
import { getNodeSensorData, getNodeStatistics } from '../api/client';
import NodeNameEditor from './NodeNameEditor';
import SensorChart from './SensorChart';
import TimeRangeSelector from './TimeRangeSelector';
import BatteryGauge from './BatteryGauge';
import SignalStrength from './SignalStrength';
import HealthStatus from './HealthStatus';

interface NodeDetailProps {
  node: Node;
  zones: Zone[];
  onBack: () => void;
  onUpdate: (node: Node) => void;
  onZoneCreated: (zone: Zone) => void;
}

function NodeDetail({ node, zones, onBack, onUpdate, onZoneCreated }: NodeDetailProps) {
  const [readings, setReadings] = useState<SensorReading[]>([]);
  const [statistics, setStatistics] = useState<NodeStatistics | null>(null);
  const [timeRange, setTimeRange] = useState<TimeRange>('24h');
  const [customRange, setCustomRange] = useState<CustomTimeRange>(() => {
    const now = Math.floor(Date.now() / 1000);
    return {
      startTime: now - 86400,
      endTime: now,
    };
  });
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState<string | null>(null);
  const [timeBounds, setTimeBounds] = useState<{ start: number; end: number } | null>(null);

  const fetchData = useCallback(async () => {
    setLoading(true);
    setError(null);
    try {
      let startTime: number | undefined;
      let endTime: number | undefined;

      const now = Math.floor(Date.now() / 1000);
      if (timeRange === 'custom') {
        startTime = customRange.startTime;
        endTime = customRange.endTime;
      } else {
        const rangeConfig = TIME_RANGES[timeRange];
        startTime = now - rangeConfig.seconds;
        endTime = now;
      }

      const [sensorData, stats] = await Promise.all([
        getNodeSensorData(node.address, {
          startTime,
          endTime,
          downsample: 500,  // Bucket-average to ~500 points for charts
        }),
        getNodeStatistics(node.address, {
          startTime,
          endTime,
        }),
      ]);

      setReadings(sensorData.readings);
      setStatistics(stats);
      setTimeBounds({ start: startTime!, end: endTime! });
    } catch (err) {
      setError(err instanceof Error ? err.message : 'Failed to fetch data');
    } finally {
      setLoading(false);
    }
  }, [node.address, timeRange, customRange]);

  useEffect(() => {
    fetchData();
  }, [fetchData]);

  const handleMetadataUpdate = (metadata: NodeMetadata) => {
    onUpdate({
      ...node,
      metadata,
    });
  };

  const displayName = node.metadata?.name || `Node ${node.address}`;

  return (
    <div>
      <button
        onClick={onBack}
        className="flex items-center space-x-2 text-gray-600 hover:text-gray-900 mb-4"
      >
        <svg className="w-5 h-5" fill="none" stroke="currentColor" viewBox="0 0 24 24">
          <path strokeLinecap="round" strokeLinejoin="round" strokeWidth={2} d="M15 19l-7-7 7-7" />
        </svg>
        <span>Back to nodes</span>
      </button>

      <div className="flex items-center justify-between mb-6">
        <div>
          <h2 className="text-2xl font-bold text-gray-900">{displayName}</h2>
          <div className="flex items-center space-x-3 mt-1">
            <span className={`inline-flex items-center px-2.5 py-0.5 rounded-full text-xs font-medium ${
              node.online ? 'bg-green-100 text-green-800' : 'bg-red-100 text-red-800'
            }`}>
              {node.online ? 'Online' : 'Offline'}
            </span>
            <span className="text-sm text-gray-500">{node.type}</span>
          </div>
        </div>
        <button onClick={fetchData} className="btn btn-secondary">
          Refresh
        </button>
      </div>

      <div className="grid grid-cols-1 lg:grid-cols-3 gap-6">
        <div className="lg:col-span-1 space-y-4">
          <NodeNameEditor node={node} zones={zones} onUpdate={handleMetadataUpdate} onZoneCreated={onZoneCreated} />

          {/* Node Status Panel */}
          {node.status && (
            <div className="card">
              <h3 className="text-lg font-medium text-gray-900 mb-4">Node Status</h3>
              <div className="space-y-4">
                {/* Battery */}
                <div>
                  <dt className="text-sm text-gray-500 mb-1">Battery</dt>
                  <dd className="flex items-center space-x-2">
                    <BatteryGauge level={node.status.battery_level} size="md" />
                    <span className="text-sm text-gray-600">
                      {getBatteryStatus(node.status.battery_level).isExternal
                        ? 'External Power'
                        : node.status.battery_level !== null
                          ? `${node.status.battery_level}%`
                          : 'Unknown'}
                    </span>
                  </dd>
                </div>

                {/* Signal Strength */}
                <div>
                  <dt className="text-sm text-gray-500 mb-1">Signal Strength</dt>
                  <dd className="flex items-center space-x-2">
                    <SignalStrength rssi={node.status.signal_strength} size="md" />
                    <span className="text-sm text-gray-600">
                      {getSignalQuality(node.status.signal_strength).label}
                    </span>
                  </dd>
                </div>

                {/* Uptime */}
                <div>
                  <dt className="text-sm text-gray-500 mb-1">Uptime</dt>
                  <dd className="text-lg font-medium text-gray-900">
                    {formatUptime(node.status.uptime_seconds)}
                  </dd>
                </div>

                {/* Health Status */}
                <div>
                  <dt className="text-sm text-gray-500 mb-1">Health</dt>
                  <dd>
                    <HealthStatus errorFlags={node.status.error_flags} size="md" showTooltip={false} />
                  </dd>
                </div>

                {/* Active Issues */}
                {node.status.error_flags !== null && node.status.error_flags !== 0 && (
                  <div className="pt-3 border-t">
                    <dt className="text-sm text-gray-500 mb-2">Active Issues</dt>
                    <dd className="flex flex-wrap gap-1.5">
                      {parseErrorFlags(node.status.error_flags).map((error) => (
                        <span
                          key={error.flag}
                          className={`inline-flex items-center px-2 py-0.5 rounded text-xs font-medium ${
                            error.severity === 'error'
                              ? 'bg-red-100 text-red-800'
                              : 'bg-yellow-100 text-yellow-800'
                          }`}
                        >
                          {error.label}
                        </span>
                      ))}
                    </dd>
                  </div>
                )}

                {/* Last Updated */}
                {node.status.updated_at && (
                  <div className="text-xs text-gray-400 pt-2">
                    Status updated: {new Date(node.status.updated_at * 1000).toLocaleString()}
                  </div>
                )}
              </div>
            </div>
          )}

          {statistics && (
            <div className="card">
              <h3 className="text-lg font-medium text-gray-900 mb-4">Statistics</h3>
              <dl className="space-y-3">
                <div>
                  <dt className="text-sm text-gray-500">Total Readings</dt>
                  <dd className="text-xl font-semibold text-gray-900">{statistics.total_readings.toLocaleString()}</dd>
                </div>
                <div className="border-t pt-3">
                  <dt className="text-sm text-gray-500 mb-2">Temperature</dt>
                  <div className="grid grid-cols-3 gap-2 text-sm">
                    <div>
                      <div className="text-gray-400">Min</div>
                      <div className="font-medium">{statistics.temperature.min_celsius?.toFixed(1) ?? '-'}C</div>
                    </div>
                    <div>
                      <div className="text-gray-400">Avg</div>
                      <div className="font-medium">{statistics.temperature.avg_celsius?.toFixed(1) ?? '-'}C</div>
                    </div>
                    <div>
                      <div className="text-gray-400">Max</div>
                      <div className="font-medium">{statistics.temperature.max_celsius?.toFixed(1) ?? '-'}C</div>
                    </div>
                  </div>
                </div>
                <div className="border-t pt-3">
                  <dt className="text-sm text-gray-500 mb-2">Humidity</dt>
                  <div className="grid grid-cols-3 gap-2 text-sm">
                    <div>
                      <div className="text-gray-400">Min</div>
                      <div className="font-medium">{statistics.humidity.min_percent?.toFixed(1) ?? '-'}%</div>
                    </div>
                    <div>
                      <div className="text-gray-400">Avg</div>
                      <div className="font-medium">{statistics.humidity.avg_percent?.toFixed(1) ?? '-'}%</div>
                    </div>
                    <div>
                      <div className="text-gray-400">Max</div>
                      <div className="font-medium">{statistics.humidity.max_percent?.toFixed(1) ?? '-'}%</div>
                    </div>
                  </div>
                </div>
              </dl>
            </div>
          )}
        </div>

        <div className="lg:col-span-2 space-y-4">
          <div className="card">
            <div className="flex items-center justify-between mb-4">
              <h3 className="text-lg font-medium text-gray-900">Sensor Data</h3>
              <TimeRangeSelector
                value={timeRange}
                onChange={setTimeRange}
                customRange={customRange}
                onCustomRangeChange={setCustomRange}
              />
            </div>

            {loading ? (
              <div className="flex items-center justify-center h-64">
                <div className="inline-block animate-spin rounded-full h-8 w-8 border-4 border-bramble-600 border-t-transparent"></div>
              </div>
            ) : error ? (
              <div className="p-4 bg-red-50 border border-red-200 rounded-md text-red-700">
                {error}
              </div>
            ) : readings.length === 0 ? (
              <div className="text-center py-12 text-gray-500">
                No sensor data available for this time range.
              </div>
            ) : (
              <div className="space-y-6">
                <SensorChart
                  readings={readings}
                  dataKey="temperature_celsius"
                  title="Temperature"
                  yAxisLabel="Celsius"
                  color="#f97316"
                  startTime={timeBounds?.start}
                  endTime={timeBounds?.end}
                />
                <SensorChart
                  readings={readings}
                  dataKey="humidity_percent"
                  title="Humidity"
                  yAxisLabel="Percent"
                  color="#3b82f6"
                  startTime={timeBounds?.start}
                  endTime={timeBounds?.end}
                />
              </div>
            )}
          </div>
        </div>
      </div>
    </div>
  );
}

export default NodeDetail;
