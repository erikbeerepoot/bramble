import { useState, useEffect } from 'react';
import type { NodeConfig } from '../types';
import { getNodeConfig, updateNodeConfig } from '../api/client';

interface NodeConfigEditorProps {
  nodeAddress: number;
}

// Validation ranges matching firmware
const VALIDATION = {
  sensor_read_interval_s: { min: 10, max: 3600, label: 'Sample Interval', unit: 's' },
  transmit_interval_s: { min: 60, max: 86400, label: 'Transmit Interval', unit: 's' },
  temperature_offset: { min: -1000, max: 1000, label: 'Temp Offset', unit: '0.01C' },
  humidity_offset: { min: -1000, max: 1000, label: 'Humidity Offset', unit: '0.01%' },
  tx_power: { min: 2, max: 20, label: 'TX Power', unit: 'dBm' },
  led_enabled: { min: 0, max: 1, label: 'Status LED', unit: '' },
};

function formatValue(key: string, value: number): string {
  if (key === 'temperature_offset') {
    return `${(value / 100).toFixed(2)} C`;
  }
  if (key === 'humidity_offset') {
    return `${(value / 100).toFixed(2)} %`;
  }
  if (key === 'led_enabled') {
    return value ? 'Enabled' : 'Disabled';
  }
  if (key === 'transmit_interval_s') {
    if (value >= 3600) {
      return `${(value / 3600).toFixed(1)} hr`;
    }
    return `${Math.round(value / 60)} min`;
  }
  if (key === 'sensor_read_interval_s') {
    if (value >= 60) {
      return `${Math.round(value / 60)} min`;
    }
    return `${value} s`;
  }
  if (key === 'tx_power') {
    return `${value} dBm`;
  }
  return `${value}`;
}

function NodeConfigEditor({ nodeAddress }: NodeConfigEditorProps) {
  const [config, setConfig] = useState<NodeConfig | null>(null);
  const [editing, setEditing] = useState(false);
  const [loading, setLoading] = useState(true);
  const [saving, setSaving] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [successMessage, setSuccessMessage] = useState<string | null>(null);

  // Form state
  const [sensorReadInterval, setSensorReadInterval] = useState(30);
  const [transmitInterval, setTransmitInterval] = useState(600);
  const [tempOffset, setTempOffset] = useState(0);
  const [humidityOffset, setHumidityOffset] = useState(0);
  const [txPower, setTxPower] = useState(20);
  const [ledEnabled, setLedEnabled] = useState(true);

  useEffect(() => {
    loadConfig();
  }, [nodeAddress]);

  const loadConfig = async () => {
    setLoading(true);
    setError(null);
    try {
      const cfg = await getNodeConfig(nodeAddress);
      setConfig(cfg);
      // Initialize form state
      setSensorReadInterval(cfg.sensor_read_interval_s);
      setTransmitInterval(cfg.transmit_interval_s);
      setTempOffset(cfg.temperature_offset);
      setHumidityOffset(cfg.humidity_offset);
      setTxPower(cfg.tx_power);
      setLedEnabled(cfg.led_enabled === 1);
    } catch (err) {
      setError(err instanceof Error ? err.message : 'Failed to load config');
    } finally {
      setLoading(false);
    }
  };

  const handleSave = async () => {
    setSaving(true);
    setError(null);
    setSuccessMessage(null);

    try {
      // Build update object with only changed values
      const updates: Record<string, number> = {};

      if (config) {
        if (sensorReadInterval !== config.sensor_read_interval_s) {
          updates.sensor_read_interval_s = sensorReadInterval;
        }
        if (transmitInterval !== config.transmit_interval_s) {
          updates.transmit_interval_s = transmitInterval;
        }
        if (tempOffset !== config.temperature_offset) {
          updates.temperature_offset = tempOffset;
        }
        if (humidityOffset !== config.humidity_offset) {
          updates.humidity_offset = humidityOffset;
        }
        if (txPower !== config.tx_power) {
          updates.tx_power = txPower;
        }
        if ((ledEnabled ? 1 : 0) !== config.led_enabled) {
          updates.led_enabled = ledEnabled ? 1 : 0;
        }
      }

      if (Object.keys(updates).length === 0) {
        setEditing(false);
        return;
      }

      const result = await updateNodeConfig(nodeAddress, updates);
      setSuccessMessage(`${result.tasks.length} update(s) queued for delivery`);
      setEditing(false);

      // Reload config to reflect cached values
      await loadConfig();
    } catch (err) {
      setError(err instanceof Error ? err.message : 'Failed to save config');
    } finally {
      setSaving(false);
    }
  };

  const handleCancel = () => {
    if (config) {
      setSensorReadInterval(config.sensor_read_interval_s);
      setTransmitInterval(config.transmit_interval_s);
      setTempOffset(config.temperature_offset);
      setHumidityOffset(config.humidity_offset);
      setTxPower(config.tx_power);
      setLedEnabled(config.led_enabled === 1);
    }
    setEditing(false);
    setError(null);
  };

  if (loading) {
    return (
      <div className="card">
        <h3 className="text-lg font-medium text-gray-900 mb-4">Node Configuration</h3>
        <p className="text-gray-500">Loading configuration...</p>
      </div>
    );
  }

  if (!editing) {
    return (
      <div className="card">
        <div className="flex items-center justify-between mb-4">
          <h3 className="text-lg font-medium text-gray-900">Node Configuration</h3>
          <button
            onClick={() => setEditing(true)}
            className="text-bramble-600 hover:text-bramble-700 text-sm font-medium"
          >
            Edit
          </button>
        </div>

        {error && (
          <div className="mb-4 p-3 bg-red-50 border border-red-200 rounded-md text-red-700 text-sm">
            {error}
          </div>
        )}

        {successMessage && (
          <div className="mb-4 p-3 bg-green-50 border border-green-200 rounded-md text-green-700 text-sm">
            {successMessage}
          </div>
        )}

        {config && (
          <dl className="grid grid-cols-2 gap-x-4 gap-y-2">
            <div>
              <dt className="text-sm text-gray-500">Sample Interval</dt>
              <dd className="text-gray-900">{formatValue('sensor_read_interval_s', config.sensor_read_interval_s)}</dd>
            </div>
            <div>
              <dt className="text-sm text-gray-500">Transmit Interval</dt>
              <dd className="text-gray-900">{formatValue('transmit_interval_s', config.transmit_interval_s)}</dd>
            </div>
            <div>
              <dt className="text-sm text-gray-500">Temperature Offset</dt>
              <dd className="text-gray-900">{formatValue('temperature_offset', config.temperature_offset)}</dd>
            </div>
            <div>
              <dt className="text-sm text-gray-500">Humidity Offset</dt>
              <dd className="text-gray-900">{formatValue('humidity_offset', config.humidity_offset)}</dd>
            </div>
            <div>
              <dt className="text-sm text-gray-500">TX Power</dt>
              <dd className="text-gray-900">{formatValue('tx_power', config.tx_power)}</dd>
            </div>
            <div>
              <dt className="text-sm text-gray-500">Status LED</dt>
              <dd className="text-gray-900">{formatValue('led_enabled', config.led_enabled)}</dd>
            </div>
          </dl>
        )}
      </div>
    );
  }

  return (
    <div className="card">
      <h3 className="text-lg font-medium text-gray-900 mb-4">Edit Node Configuration</h3>

      {error && (
        <div className="mb-4 p-3 bg-red-50 border border-red-200 rounded-md text-red-700 text-sm">
          {error}
        </div>
      )}

      <div className="space-y-4">
        <div className="grid grid-cols-2 gap-4">
          <div>
            <label htmlFor="sensorInterval" className="block text-sm font-medium text-gray-700 mb-1">
              Sample Interval (seconds)
            </label>
            <input
              id="sensorInterval"
              type="number"
              min={VALIDATION.sensor_read_interval_s.min}
              max={VALIDATION.sensor_read_interval_s.max}
              value={sensorReadInterval}
              onChange={(e) => setSensorReadInterval(parseInt(e.target.value) || 30)}
              className="input w-full"
            />
            <p className="mt-1 text-xs text-gray-500">{VALIDATION.sensor_read_interval_s.min}-{VALIDATION.sensor_read_interval_s.max}s</p>
          </div>

          <div>
            <label htmlFor="transmitInterval" className="block text-sm font-medium text-gray-700 mb-1">
              Transmit Interval (seconds)
            </label>
            <input
              id="transmitInterval"
              type="number"
              min={VALIDATION.transmit_interval_s.min}
              max={VALIDATION.transmit_interval_s.max}
              value={transmitInterval}
              onChange={(e) => setTransmitInterval(parseInt(e.target.value) || 600)}
              className="input w-full"
            />
            <p className="mt-1 text-xs text-gray-500">{VALIDATION.transmit_interval_s.min}-{VALIDATION.transmit_interval_s.max}s</p>
          </div>
        </div>

        <div className="grid grid-cols-2 gap-4">
          <div>
            <label htmlFor="tempOffset" className="block text-sm font-medium text-gray-700 mb-1">
              Temperature Offset (0.01C units)
            </label>
            <input
              id="tempOffset"
              type="number"
              min={VALIDATION.temperature_offset.min}
              max={VALIDATION.temperature_offset.max}
              value={tempOffset}
              onChange={(e) => setTempOffset(parseInt(e.target.value) || 0)}
              className="input w-full"
            />
            <p className="mt-1 text-xs text-gray-500">e.g., 50 = +0.50C</p>
          </div>

          <div>
            <label htmlFor="humidityOffset" className="block text-sm font-medium text-gray-700 mb-1">
              Humidity Offset (0.01% units)
            </label>
            <input
              id="humidityOffset"
              type="number"
              min={VALIDATION.humidity_offset.min}
              max={VALIDATION.humidity_offset.max}
              value={humidityOffset}
              onChange={(e) => setHumidityOffset(parseInt(e.target.value) || 0)}
              className="input w-full"
            />
            <p className="mt-1 text-xs text-gray-500">e.g., -25 = -0.25%</p>
          </div>
        </div>

        <div className="grid grid-cols-2 gap-4">
          <div>
            <label htmlFor="txPower" className="block text-sm font-medium text-gray-700 mb-1">
              TX Power (dBm)
            </label>
            <input
              id="txPower"
              type="number"
              min={VALIDATION.tx_power.min}
              max={VALIDATION.tx_power.max}
              value={txPower}
              onChange={(e) => setTxPower(parseInt(e.target.value) || 20)}
              className="input w-full"
            />
            <p className="mt-1 text-xs text-gray-500">{VALIDATION.tx_power.min}-{VALIDATION.tx_power.max} dBm</p>
          </div>

          <div>
            <label className="block text-sm font-medium text-gray-700 mb-1">
              Status LED
            </label>
            <div className="flex items-center space-x-3 mt-2">
              <label className="inline-flex items-center">
                <input
                  type="radio"
                  name="ledEnabled"
                  checked={ledEnabled}
                  onChange={() => setLedEnabled(true)}
                  className="form-radio text-bramble-600"
                />
                <span className="ml-2">Enabled</span>
              </label>
              <label className="inline-flex items-center">
                <input
                  type="radio"
                  name="ledEnabled"
                  checked={!ledEnabled}
                  onChange={() => setLedEnabled(false)}
                  className="form-radio text-bramble-600"
                />
                <span className="ml-2">Disabled</span>
              </label>
            </div>
          </div>
        </div>

        <div className="flex space-x-3 pt-2">
          <button
            onClick={handleSave}
            disabled={saving}
            className="btn btn-primary"
          >
            {saving ? 'Saving...' : 'Save'}
          </button>
          <button
            onClick={handleCancel}
            disabled={saving}
            className="btn btn-secondary"
          >
            Cancel
          </button>
        </div>

        <p className="text-sm text-gray-500 mt-2">
          Changes will be queued and delivered when the node next checks in.
        </p>
      </div>
    </div>
  );
}

export default NodeConfigEditor;
