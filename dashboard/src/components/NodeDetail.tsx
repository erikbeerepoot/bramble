import { useState, useEffect, useCallback } from 'react';
import type { Node, NodeStatistics, SensorReading, TimeRange, NodeMetadata } from '../types';
import { TIME_RANGES } from '../types';
import { getNodeSensorData, getNodeStatistics } from '../api/client';
import NodeNameEditor from './NodeNameEditor';
import SensorChart from './SensorChart';
import TimeRangeSelector from './TimeRangeSelector';

interface NodeDetailProps {
  node: Node;
  onBack: () => void;
  onUpdate: (node: Node) => void;
}

function NodeDetail({ node, onBack, onUpdate }: NodeDetailProps) {
  const [readings, setReadings] = useState<SensorReading[]>([]);
  const [statistics, setStatistics] = useState<NodeStatistics | null>(null);
  const [timeRange, setTimeRange] = useState<TimeRange>('24h');
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState<string | null>(null);

  const fetchData = useCallback(async () => {
    setLoading(true);
    setError(null);
    try {
      const now = Math.floor(Date.now() / 1000);
      const rangeConfig = TIME_RANGES[timeRange as keyof typeof TIME_RANGES];
      const startTime = rangeConfig ? now - rangeConfig.seconds : undefined;

      const [sensorData, stats] = await Promise.all([
        getNodeSensorData(node.address, {
          startTime,
          limit: 5000,
        }),
        getNodeStatistics(node.address),
      ]);

      setReadings(sensorData.readings);
      setStatistics(stats);
    } catch (err) {
      setError(err instanceof Error ? err.message : 'Failed to fetch data');
    } finally {
      setLoading(false);
    }
  }, [node.address, timeRange]);

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
          <NodeNameEditor node={node} onUpdate={handleMetadataUpdate} />

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
              <TimeRangeSelector value={timeRange} onChange={setTimeRange} />
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
                />
                <SensorChart
                  readings={readings}
                  dataKey="humidity_percent"
                  title="Humidity"
                  yAxisLabel="Percent"
                  color="#3b82f6"
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
