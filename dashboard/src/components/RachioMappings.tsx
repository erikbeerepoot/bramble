import { useCallback, useEffect, useMemo, useState } from 'react';
import type { Node, RachioMapping } from '../types';
import { useAppContext } from '../App';
import {
  getRachioMappings,
  upsertRachioMapping,
  deleteRachioMapping,
} from '../api/client';

// Nodes that report a valve_count > 0 can be driven by a Rachio zone.
function hasValves(node: Node): boolean {
  return node.valve_count != null && node.valve_count > 0;
}

// Valve indices 0..count-1 for a node.
function valveIndices(count: number): number[] {
  return Array.from({ length: count }, (_, i) => i);
}

function valveLabel(node: Node | undefined, valve: number): string {
  const name = node?.valves?.[String(valve)];
  return name ? `${name} (valve ${valve})` : `Valve ${valve}`;
}

function nodeLabel(node: Node): string {
  return node.metadata?.name || `Node ${node.address}`;
}

function formatDuration(seconds: number): string {
  if (seconds % 60 === 0) {
    const minutes = seconds / 60;
    return `${minutes} min`;
  }
  return `${seconds} s`;
}

interface FormState {
  rachioDeviceId: string;
  rachioZone: string;
  brambleDeviceId: string;
  brambleValve: string;
  durationSeconds: string;
  enabled: boolean;
}

const EMPTY_FORM: FormState = {
  rachioDeviceId: '',
  rachioZone: '',
  brambleDeviceId: '',
  brambleValve: '',
  durationSeconds: '900',
  enabled: true,
};

function RachioMappings() {
  const { nodes } = useAppContext();
  const valveNodes = useMemo(() => nodes.filter(hasValves), [nodes]);
  const nodeById = useMemo(() => {
    const map = new Map<string, Node>();
    for (const node of nodes) map.set(node.device_id, node);
    return map;
  }, [nodes]);

  const [mappings, setMappings] = useState<RachioMapping[]>([]);
  const [loading, setLoading] = useState(true);
  const [loadError, setLoadError] = useState<string | null>(null);

  const [showForm, setShowForm] = useState(false);
  // When editing an existing mapping, its (device, zone) is the identity and
  // cannot be changed — editing those fields would orphan the old row rather
  // than update it, so we lock them and re-upsert on the same key.
  const [editingKey, setEditingKey] = useState<string | null>(null);
  const [form, setForm] = useState<FormState>(EMPTY_FORM);
  const [saving, setSaving] = useState(false);
  const [formError, setFormError] = useState<string | null>(null);

  const fetchMappings = useCallback(async () => {
    try {
      setLoadError(null);
      const response = await getRachioMappings();
      setMappings(response.mappings);
    } catch (err) {
      setLoadError(err instanceof Error ? err.message : 'Failed to load mappings');
    } finally {
      setLoading(false);
    }
  }, []);

  useEffect(() => {
    fetchMappings();
  }, [fetchMappings]);

  const selectedNode = form.brambleDeviceId ? nodeById.get(form.brambleDeviceId) : undefined;
  const selectedValveCount = selectedNode?.valve_count ?? 0;

  const resetForm = () => {
    setForm(EMPTY_FORM);
    setEditingKey(null);
    setFormError(null);
    setShowForm(false);
  };

  const startAdd = () => {
    setForm(EMPTY_FORM);
    setEditingKey(null);
    setFormError(null);
    setShowForm(true);
  };

  const startEdit = (mapping: RachioMapping) => {
    setForm({
      rachioDeviceId: mapping.rachio_device_id,
      rachioZone: String(mapping.rachio_zone_number),
      brambleDeviceId: mapping.bramble_device_id,
      brambleValve: String(mapping.bramble_valve),
      durationSeconds: String(mapping.duration_seconds),
      enabled: mapping.enabled,
    });
    setEditingKey(`${mapping.rachio_device_id}:${mapping.rachio_zone_number}`);
    setFormError(null);
    setShowForm(true);
  };

  const handleSave = async () => {
    const zone = Number(form.rachioZone);
    const valve = Number(form.brambleValve);
    const duration = Number(form.durationSeconds);

    if (!form.rachioDeviceId.trim()) {
      setFormError('Rachio controller ID is required.');
      return;
    }
    if (!Number.isInteger(zone) || zone < 0) {
      setFormError('Rachio zone number must be a non-negative integer.');
      return;
    }
    if (!form.brambleDeviceId) {
      setFormError('Select a Bramble node.');
      return;
    }
    if (!Number.isInteger(valve) || valve < 0 || valve >= selectedValveCount) {
      setFormError('Select a valve on the chosen node.');
      return;
    }
    if (!Number.isInteger(duration) || duration < 1 || duration > 7200) {
      setFormError('Fallback duration must be between 1 and 7200 seconds.');
      return;
    }

    setSaving(true);
    setFormError(null);
    try {
      await upsertRachioMapping({
        rachio_device_id: form.rachioDeviceId.trim(),
        rachio_zone_number: zone,
        bramble_device_id: form.brambleDeviceId,
        bramble_valve: valve,
        duration_seconds: duration,
        enabled: form.enabled,
      });
      await fetchMappings();
      resetForm();
    } catch (err) {
      setFormError(err instanceof Error ? err.message : 'Failed to save mapping');
    } finally {
      setSaving(false);
    }
  };

  const handleDelete = async (mapping: RachioMapping) => {
    const node = nodeById.get(mapping.bramble_device_id);
    const target = node ? nodeLabel(node) : `device ${mapping.bramble_device_id}`;
    if (
      !window.confirm(
        `Delete the mapping from Rachio zone ${mapping.rachio_zone_number} → ${target}?`
      )
    ) {
      return;
    }
    try {
      await deleteRachioMapping(mapping.rachio_device_id, mapping.rachio_zone_number);
      await fetchMappings();
    } catch (err) {
      setLoadError(err instanceof Error ? err.message : 'Failed to delete mapping');
    }
  };

  return (
    <div className="card mt-4">
      <div className="flex items-start justify-between mb-2">
        <h3 className="text-lg font-medium text-gray-900">Rachio Integration</h3>
        {!showForm && (
          <button onClick={startAdd} className="btn btn-primary" disabled={loading}>
            Add mapping
          </button>
        )}
      </div>
      <p className="text-sm text-gray-500 mb-4">
        Map a Rachio controller zone to a Bramble valve. When the Rachio zone starts, the mapped
        valve runs; when it stops, the valve closes. Find your controller ID and zone numbers with{' '}
        <code>scripts/register_rachio_webhook.py list</code>.
      </p>

      {loadError && (
        <div className="p-3 mb-4 rounded-md text-sm bg-red-50 border border-red-200 text-red-700">
          {loadError}
        </div>
      )}

      {showForm && (
        <div className="border border-gray-200 rounded-md p-4 mb-4 bg-gray-50">
          <h4 className="text-sm font-semibold text-gray-900 mb-3">
            {editingKey ? 'Edit mapping' : 'New mapping'}
          </h4>
          <div className="grid grid-cols-1 sm:grid-cols-2 gap-4">
            <div>
              <label className="block text-sm font-medium text-gray-700 mb-1">
                Rachio controller ID *
              </label>
              <input
                type="text"
                value={form.rachioDeviceId}
                onChange={(e) => setForm({ ...form, rachioDeviceId: e.target.value })}
                placeholder="controller uuid"
                className="input w-full"
                disabled={editingKey !== null}
              />
            </div>
            <div>
              <label className="block text-sm font-medium text-gray-700 mb-1">
                Rachio zone number *
              </label>
              <input
                type="number"
                min={0}
                value={form.rachioZone}
                onChange={(e) => setForm({ ...form, rachioZone: e.target.value })}
                className="input w-full"
                disabled={editingKey !== null}
              />
            </div>
            <div>
              <label className="block text-sm font-medium text-gray-700 mb-1">Bramble node *</label>
              <select
                value={form.brambleDeviceId}
                onChange={(e) =>
                  setForm({ ...form, brambleDeviceId: e.target.value, brambleValve: '' })
                }
                className="input w-full"
              >
                <option value="">Select node…</option>
                {valveNodes.map((node) => (
                  <option key={node.device_id} value={node.device_id}>
                    {nodeLabel(node)}
                  </option>
                ))}
              </select>
            </div>
            <div>
              <label className="block text-sm font-medium text-gray-700 mb-1">Valve *</label>
              <select
                value={form.brambleValve}
                onChange={(e) => setForm({ ...form, brambleValve: e.target.value })}
                className="input w-full"
                disabled={!selectedNode}
              >
                <option value="">Select valve…</option>
                {valveIndices(selectedValveCount).map((v) => (
                  <option key={v} value={v}>
                    {valveLabel(selectedNode, v)}
                  </option>
                ))}
              </select>
            </div>
            <div>
              <label className="block text-sm font-medium text-gray-700 mb-1">
                Fallback duration (seconds) *
              </label>
              <input
                type="number"
                min={1}
                max={7200}
                value={form.durationSeconds}
                onChange={(e) => setForm({ ...form, durationSeconds: e.target.value })}
                className="input w-full"
              />
              <p className="mt-1 text-xs text-gray-500">
                Used only when Rachio doesn't report its own run length (1–7200).
              </p>
            </div>
            <div className="flex items-center">
              <label className="flex items-center gap-2 text-sm font-medium text-gray-700">
                <input
                  type="checkbox"
                  checked={form.enabled}
                  onChange={(e) => setForm({ ...form, enabled: e.target.checked })}
                  className="h-4 w-4"
                />
                Enabled
              </label>
            </div>
          </div>

          {formError && (
            <div className="p-3 mt-4 rounded-md text-sm bg-red-50 border border-red-200 text-red-700">
              {formError}
            </div>
          )}

          <div className="flex gap-3 mt-4">
            <button onClick={handleSave} disabled={saving} className="btn btn-primary">
              {saving ? 'Saving…' : 'Save mapping'}
            </button>
            <button onClick={resetForm} disabled={saving} className="btn btn-secondary">
              Cancel
            </button>
          </div>
        </div>
      )}

      {loading ? (
        <p className="text-sm text-gray-500">Loading mappings…</p>
      ) : mappings.length === 0 ? (
        <p className="text-sm text-gray-500">
          No mappings yet. Add one to have a Rachio zone drive a Bramble valve.
        </p>
      ) : (
        <div className="overflow-x-auto">
          <table className="min-w-full text-sm">
            <thead>
              <tr className="text-left text-gray-500 border-b border-gray-200">
                <th className="py-2 pr-4 font-medium">Rachio zone</th>
                <th className="py-2 pr-4 font-medium">Bramble valve</th>
                <th className="py-2 pr-4 font-medium">Fallback duration</th>
                <th className="py-2 pr-4 font-medium">Status</th>
                <th className="py-2 pr-4 font-medium"></th>
              </tr>
            </thead>
            <tbody>
              {mappings.map((mapping) => {
                const node = nodeById.get(mapping.bramble_device_id);
                return (
                  <tr
                    key={`${mapping.rachio_device_id}:${mapping.rachio_zone_number}`}
                    className="border-b border-gray-100"
                  >
                    <td className="py-2 pr-4">
                      <div className="text-gray-900">Zone {mapping.rachio_zone_number}</div>
                      <div className="text-xs text-gray-400 font-mono truncate max-w-[12rem]">
                        {mapping.rachio_device_id}
                      </div>
                    </td>
                    <td className="py-2 pr-4">
                      <div className="text-gray-900">
                        {node ? nodeLabel(node) : `Device ${mapping.bramble_device_id}`}
                      </div>
                      <div className="text-xs text-gray-400">
                        {valveLabel(node, mapping.bramble_valve)}
                      </div>
                    </td>
                    <td className="py-2 pr-4 text-gray-700">
                      {formatDuration(mapping.duration_seconds)}
                    </td>
                    <td className="py-2 pr-4">
                      {mapping.enabled ? (
                        <span className="inline-block px-2 py-0.5 rounded-full text-xs bg-green-50 text-green-700 border border-green-200">
                          Enabled
                        </span>
                      ) : (
                        <span className="inline-block px-2 py-0.5 rounded-full text-xs bg-gray-100 text-gray-500 border border-gray-200">
                          Disabled
                        </span>
                      )}
                    </td>
                    <td className="py-2 pr-4">
                      <div className="flex gap-3">
                        <button
                          onClick={() => startEdit(mapping)}
                          className="text-sm text-blue-600 hover:text-blue-800"
                        >
                          Edit
                        </button>
                        <button
                          onClick={() => handleDelete(mapping)}
                          className="text-sm text-red-600 hover:text-red-800"
                        >
                          Delete
                        </button>
                      </div>
                    </td>
                  </tr>
                );
              })}
            </tbody>
          </table>
        </div>
      )}
    </div>
  );
}

export default RachioMappings;
