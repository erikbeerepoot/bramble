import { useState, useEffect, useCallback, useRef, useMemo } from 'react';
import type { Node, NodeStatistics, SensorReading, TimeRange, NodeMetadata, CustomTimeRange, Zone } from '../types';
import { TIME_RANGES, parseErrorFlags, formatUptime, getSignalQuality, getBatteryStatus, getHealthStatus } from '../types';
import { getNodeSensorData, getNodeStatistics, deleteNode } from '../api/client';
import NodeNameEditor from './NodeNameEditor';
import SensorChart from './SensorChart';
import TimeRangeSelector from './TimeRangeSelector';
import BatteryGauge from './BatteryGauge';
import SignalStrength from './SignalStrength';
import HealthStatus from './HealthStatus';
import BacklogStatus from './BacklogStatus';

interface NodeDetailProps {
  node: Node;
  zones: Zone[];
  onBack: () => void;
  onUpdate: (node: Node) => void;
  onDelete: (deviceId: number) => void;
  onZoneCreated: (zone: Zone) => void;
}

function NodeDetail({ node, zones, onBack, onUpdate, onDelete, onZoneCreated }: NodeDetailProps) {
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
  const [loadingSensorData, setLoadingSensorData] = useState(true);
  const [loadingStatistics, setLoadingStatistics] = useState(true);
  const [error, setError] = useState<string | null>(null);
  const [timeBounds, setTimeBounds] = useState<{ start: number; end: number } | null>(null);
  const [showDeleteConfirm, setShowDeleteConfirm] = useState(false);
  const [deleting, setDeleting] = useState(false);
  const [dangerZoneExpanded, setDangerZoneExpanded] = useState(false);
  const abortControllerRef = useRef<AbortController | null>(null);

  const fetchData = useCallback(async () => {
    // Abort any in-flight request before starting a new one
    if (abortControllerRef.current) {
      abortControllerRef.current.abort();
    }
    const controller = new AbortController();
    abortControllerRef.current = controller;
    const { signal } = controller;

    setLoadingSensorData(true);
    setLoadingStatistics(true);
    setError(null);

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

    setTimeBounds({ start: startTime!, end: endTime! });

    // Fire both requests in parallel but update UI as each resolves
    const sensorDataPromise = getNodeSensorData(node.device_id, {
      startTime,
      endTime,
      downsample: 500,
      signal,
    }).then((sensorData) => {
      setReadings(sensorData.readings);
      setLoadingSensorData(false);
    });

    const statisticsPromise = getNodeStatistics(node.device_id, {
      startTime,
      endTime,
      signal,
    }).then((stats) => {
      setStatistics(stats);
      setLoadingStatistics(false);
    });

    // Wait for both to settle so we can surface any errors
    const results = await Promise.allSettled([sensorDataPromise, statisticsPromise]);
    if (signal.aborted) return; // Superseded by a newer request

    const failures = results.filter((r): r is PromiseRejectedResult => r.status === 'rejected');
    if (failures.length > 0) {
      const reason = failures[0].reason;
      if (reason instanceof DOMException && reason.name === 'AbortError') return;
      setError(reason instanceof Error ? reason.message : 'Failed to fetch data');
    }
  }, [node.device_id, timeRange, customRange]);

  useEffect(() => {
    fetchData();
    return () => {
      abortControllerRef.current?.abort();
    };
  }, [fetchData]);

  const handleMetadataUpdate = (metadata: NodeMetadata) => {
    onUpdate({
      ...node,
      metadata,
    });
  };

  const handleDelete = async () => {
    setDeleting(true);
    try {
      await deleteNode(node.device_id);
      onDelete(node.device_id);
      onBack();
    } catch (err) {
      setError(err instanceof Error ? err.message : 'Failed to delete node');
      setShowDeleteConfirm(false);
    } finally {
      setDeleting(false);
    }
  };

  const displayName = node.metadata?.name || `Node ${BigInt(node.device_id).toString(16).toUpperCase()}`;
  const currentZone = zones.find(z => z.id === node.metadata?.zone_id);
  const healthStatus = useMemo(() => getHealthStatus(node.status?.error_flags ?? null), [node.status?.error_flags]);
  const isHealthy = healthStatus === 'healthy';
  const [statusExpanded, setStatusExpanded] = useState(!isHealthy);

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

      <div className="flex items-start justify-between gap-4 mb-6">
        <div className="min-w-0">
          <h2 className="text-xl sm:text-2xl font-bold text-gray-900 truncate">{displayName}</h2>
          {currentZone && (
            <span
              className="inline-flex items-center gap-1.5 px-2.5 py-0.5 rounded-full text-xs font-medium mt-1"
              style={{ backgroundColor: currentZone.color + '18', color: currentZone.color }}
            >
              <span
                className="w-2 h-2 rounded-full"
                style={{ backgroundColor: currentZone.color }}
              />
              {currentZone.name}
            </span>
          )}
          <div className="flex items-center space-x-3 mt-1">
            <span className={`inline-flex items-center px-2.5 py-0.5 rounded-full text-xs font-medium ${
              node.online ? 'bg-green-100 text-green-800' : 'bg-red-100 text-red-800'
            }`}>
              {node.online ? 'Online' : 'Offline'}
            </span>
            <span className="text-sm text-gray-500">{node.type}</span>
          </div>
        </div>
        <button onClick={fetchData} className="btn btn-secondary shrink-0">
          Refresh
        </button>
      </div>

      <div className="grid grid-cols-1 lg:grid-cols-3 gap-6">
        <div className="lg:col-span-1 space-y-4">
          {/* 1. Statistics */}
          {loadingStatistics ? (
            <div className="card animate-pulse">
              <div className="h-5 w-24 bg-gray-200 rounded mb-4" />
              <div className="space-y-3">
                <div>
                  <div className="h-3 w-24 bg-gray-200 rounded mb-1" />
                  <div className="h-6 w-16 bg-gray-200 rounded" />
                </div>
                <div className="border-t pt-3">
                  <div className="h-3 w-20 bg-gray-200 rounded mb-2" />
                  <div className="grid grid-cols-3 gap-2">
                    <div><div className="h-3 w-8 bg-gray-200 rounded mb-1" /><div className="h-4 w-12 bg-gray-200 rounded" /></div>
                    <div><div className="h-3 w-8 bg-gray-200 rounded mb-1" /><div className="h-4 w-12 bg-gray-200 rounded" /></div>
                    <div><div className="h-3 w-8 bg-gray-200 rounded mb-1" /><div className="h-4 w-12 bg-gray-200 rounded" /></div>
                  </div>
                </div>
                <div className="border-t pt-3">
                  <div className="h-3 w-16 bg-gray-200 rounded mb-2" />
                  <div className="grid grid-cols-3 gap-2">
                    <div><div className="h-3 w-8 bg-gray-200 rounded mb-1" /><div className="h-4 w-12 bg-gray-200 rounded" /></div>
                    <div><div className="h-3 w-8 bg-gray-200 rounded mb-1" /><div className="h-4 w-12 bg-gray-200 rounded" /></div>
                    <div><div className="h-3 w-8 bg-gray-200 rounded mb-1" /><div className="h-4 w-12 bg-gray-200 rounded" /></div>
                  </div>
                </div>
              </div>
            </div>
          ) : statistics && (
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

          {/* 2. Node Info */}
          <NodeNameEditor node={node} zones={zones} onUpdate={handleMetadataUpdate} onZoneCreated={onZoneCreated} />

          {/* 3. Node Status Panel - collapsible */}
          {node.status && (
            <div className="card">
              <button
                onClick={() => setStatusExpanded(!statusExpanded)}
                className="w-full flex items-center justify-between"
              >
                <div className="flex items-center space-x-2">
                  <h3 className="text-lg font-medium text-gray-900">Node Status</h3>
                  <HealthStatus errorFlags={node.status.error_flags} size="sm" showTooltip={false} />
                </div>
                <svg
                  className={`w-5 h-5 text-gray-400 transition-transform ${statusExpanded ? 'rotate-180' : ''}`}
                  fill="none"
                  stroke="currentColor"
                  viewBox="0 0 24 24"
                >
                  <path strokeLinecap="round" strokeLinejoin="round" strokeWidth={2} d="M19 9l-7 7-7-7" />
                </svg>
              </button>

              {statusExpanded && (
                <div className="space-y-4 mt-4">
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

                  {/* Pending Records */}
                  {node.status.pending_records !== null && node.status.pending_records !== undefined && (
                    <div>
                      <dt className="text-sm text-gray-500 mb-1">Unsent Records</dt>
                      <dd className="flex items-center space-x-2">
                        <BacklogStatus pendingRecords={node.status.pending_records} size="md" />
                      </dd>
                    </div>
                  )}

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
              )}
            </div>
          )}

          {/* 4. Danger Zone - collapsible, collapsed by default */}
          <div className="card">
            <button
              onClick={() => setDangerZoneExpanded(!dangerZoneExpanded)}
              className="w-full flex items-center justify-between"
            >
              <h3 className="text-lg font-medium text-gray-900">Danger Zone</h3>
              <svg
                className={`w-5 h-5 text-gray-400 transition-transform ${dangerZoneExpanded ? 'rotate-180' : ''}`}
                fill="none"
                stroke="currentColor"
                viewBox="0 0 24 24"
              >
                <path strokeLinecap="round" strokeLinejoin="round" strokeWidth={2} d="M19 9l-7 7-7-7" />
              </svg>
            </button>

            {dangerZoneExpanded && (
              <div className="mt-4">
                <p className="text-sm text-gray-500 mb-3">
                  Permanently delete this node and all its sensor data history.
                </p>
                <button
                  onClick={() => setShowDeleteConfirm(true)}
                  className="btn bg-red-600 text-white hover:bg-red-700 w-full"
                >
                  Delete Node
                </button>
              </div>
            )}
          </div>
        </div>

        <div className="lg:col-span-2 space-y-4">
          <div className="card">
            <div className="flex flex-col sm:flex-row sm:items-center sm:justify-between gap-3 mb-4">
              <h3 className="text-lg font-medium text-gray-900">Sensor Data</h3>
              <TimeRangeSelector
                value={timeRange}
                onChange={setTimeRange}
                customRange={customRange}
                onCustomRangeChange={setCustomRange}
              />
            </div>

            {loadingSensorData ? (
              <div className="space-y-6 animate-pulse">
                {/* Temperature chart skeleton */}
                <div>
                  <div className="h-4 w-28 bg-gray-200 rounded mb-3" />
                  <div className="h-48 bg-gray-200 rounded" />
                </div>
                {/* Humidity chart skeleton */}
                <div>
                  <div className="h-4 w-20 bg-gray-200 rounded mb-3" />
                  <div className="h-48 bg-gray-200 rounded" />
                </div>
              </div>
            ) : error && readings.length === 0 ? (
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

      {/* Delete Confirmation Dialog */}
      {showDeleteConfirm && (
        <div className="fixed inset-0 bg-black bg-opacity-50 flex items-center justify-center z-50">
          <div className="bg-white rounded-lg p-6 max-w-md mx-4 shadow-xl">
            <h3 className="text-lg font-medium text-gray-900 mb-2">Delete Node?</h3>
            <p className="text-sm text-gray-500 mb-4">
              This will permanently delete <strong>{displayName}</strong> and all its sensor data history.
              This action cannot be undone.
            </p>
            <div className="flex space-x-3 justify-end">
              <button
                onClick={() => setShowDeleteConfirm(false)}
                disabled={deleting}
                className="btn btn-secondary"
              >
                Cancel
              </button>
              <button
                onClick={handleDelete}
                disabled={deleting}
                className="btn bg-red-600 text-white hover:bg-red-700 disabled:opacity-50"
              >
                {deleting ? 'Deleting...' : 'Delete'}
              </button>
            </div>
          </div>
        </div>
      )}
    </div>
  );
}

export default NodeDetail;
