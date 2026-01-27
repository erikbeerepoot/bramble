import { useState } from 'react';
import type { Node, NodeMetadata, Zone } from '../types';
import { updateNodeMetadata, setNodeZone } from '../api/client';
import CreateZoneModal from './CreateZoneModal';

interface NodeNameEditorProps {
  node: Node;
  zones: Zone[];
  onUpdate: (metadata: NodeMetadata) => void;
  onZoneCreated: (zone: Zone) => void;
}

function NodeNameEditor({ node, zones, onUpdate, onZoneCreated }: NodeNameEditorProps) {
  const [editing, setEditing] = useState(false);
  const [name, setName] = useState(node.metadata?.name || '');
  const [notes, setNotes] = useState(node.metadata?.notes || '');
  const [selectedZoneId, setSelectedZoneId] = useState<number | null>(node.metadata?.zone_id ?? null);
  const [saving, setSaving] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [showCreateZone, setShowCreateZone] = useState(false);
  const [expanded, setExpanded] = useState(false);

  const currentZone = zones.find(z => z.id === node.metadata?.zone_id);

  const handleSave = async () => {
    setSaving(true);
    setError(null);
    try {
      // Update zone if changed
      const currentZoneId = node.metadata?.zone_id ?? null;
      if (selectedZoneId !== currentZoneId) {
        await setNodeZone(node.address, selectedZoneId);
      }

      // Update other metadata
      const updated = await updateNodeMetadata(node.address, {
        name: name || null,
        notes: notes || null,
      });
      onUpdate(updated);
      setEditing(false);
    } catch (err) {
      setError(err instanceof Error ? err.message : 'Failed to save');
    } finally {
      setSaving(false);
    }
  };

  const handleCancel = () => {
    setName(node.metadata?.name || '');
    setNotes(node.metadata?.notes || '');
    setSelectedZoneId(node.metadata?.zone_id ?? null);
    setEditing(false);
    setError(null);
  };

  const handleZoneCreated = (zone: Zone) => {
    onZoneCreated(zone);
    setSelectedZoneId(zone.id);
    setShowCreateZone(false);
  };

  if (!editing) {
    return (
      <div className="card">
        <button
          onClick={() => setExpanded(!expanded)}
          className="group w-full flex items-center justify-between"
        >
          <div className="flex items-center gap-1.5">
            <h3 className="text-lg font-medium text-gray-900">Node Info</h3>
            <svg
              onClick={(e) => { e.stopPropagation(); setExpanded(true); setEditing(true); }}
              className="w-4 h-4 text-gray-400 hover:text-bramble-600 cursor-pointer opacity-0 group-hover:opacity-100 transition-opacity"
              fill="none"
              stroke="currentColor"
              viewBox="0 0 24 24"
            >
              <path strokeLinecap="round" strokeLinejoin="round" strokeWidth={2} d="M15.232 5.232l3.536 3.536m-2.036-5.036a2.5 2.5 0 113.536 3.536L6.5 21.036H3v-3.572L16.732 3.732z" />
            </svg>
          </div>
          <svg
            className={`w-5 h-5 text-gray-400 transition-transform ${expanded ? 'rotate-180' : ''}`}
            fill="none"
            stroke="currentColor"
            viewBox="0 0 24 24"
          >
            <path strokeLinecap="round" strokeLinejoin="round" strokeWidth={2} d="M19 9l-7 7-7-7" />
          </svg>
        </button>

        {expanded && (
          <dl className="space-y-2 mt-4">
            <div>
              <dt className="text-sm text-gray-500">Name</dt>
              <dd className="text-gray-900">{node.metadata?.name || <span className="text-gray-400 italic">Not set</span>}</dd>
            </div>
            <div>
              <dt className="text-sm text-gray-500">Zone</dt>
              <dd className="text-gray-900">
                {currentZone ? (
                  <span className="flex items-center space-x-2">
                    <span
                      className="w-3 h-3 rounded-full"
                      style={{ backgroundColor: currentZone.color }}
                    />
                    <span>{currentZone.name}</span>
                  </span>
                ) : (
                  <span className="text-gray-400 italic">Not assigned</span>
                )}
              </dd>
            </div>
            <div>
              <dt className="text-sm text-gray-500">Notes</dt>
              <dd className="text-gray-900">{node.metadata?.notes || <span className="text-gray-400 italic">Not set</span>}</dd>
            </div>
            <div>
              <dt className="text-sm text-gray-500">Address</dt>
              <dd className="text-gray-900 font-mono">{node.address}</dd>
            </div>
            {node.device_id && (
              <div>
                <dt className="text-sm text-gray-500">Device ID</dt>
                <dd className="text-gray-900 font-mono text-sm">{node.device_id.toString(16).toUpperCase()}</dd>
              </div>
            )}
            {node.firmware_version && (
              <div>
                <dt className="text-sm text-gray-500">Firmware</dt>
                <dd className="text-gray-900 font-mono text-sm">v{node.firmware_version}</dd>
              </div>
            )}
          </dl>
        )}
      </div>
    );
  }

  return (
    <div className="card">
      <h3 className="text-lg font-medium text-gray-900 mb-4">Edit Node Info</h3>

      {error && (
        <div className="mb-4 p-3 bg-red-50 border border-red-200 rounded-md text-red-700 text-sm">
          {error}
        </div>
      )}

      <div className="space-y-4">
        <div>
          <label htmlFor="name" className="block text-sm font-medium text-gray-700 mb-1">
            Name
          </label>
          <input
            id="name"
            type="text"
            value={name}
            onChange={(e) => setName(e.target.value)}
            placeholder="e.g., Greenhouse Sensor"
            className="input w-full"
          />
        </div>

        <div>
          <label htmlFor="zone" className="block text-sm font-medium text-gray-700 mb-1">
            Zone
          </label>
          <div className="flex space-x-2">
            <select
              id="zone"
              value={selectedZoneId ?? ''}
              onChange={(e) => setSelectedZoneId(e.target.value ? parseInt(e.target.value) : null)}
              className="input flex-1"
            >
              <option value="">No zone</option>
              {zones.map(zone => (
                <option key={zone.id} value={zone.id}>
                  {zone.name}
                </option>
              ))}
            </select>
            <button
              type="button"
              onClick={() => setShowCreateZone(true)}
              className="btn btn-secondary text-sm"
              title="Create new zone"
            >
              + New
            </button>
          </div>
          {selectedZoneId && (
            <div className="mt-1 flex items-center space-x-2 text-sm text-gray-500">
              <span
                className="w-3 h-3 rounded-full"
                style={{ backgroundColor: zones.find(z => z.id === selectedZoneId)?.color }}
              />
              <span>{zones.find(z => z.id === selectedZoneId)?.name}</span>
            </div>
          )}
        </div>

        <div>
          <label htmlFor="notes" className="block text-sm font-medium text-gray-700 mb-1">
            Notes
          </label>
          <textarea
            id="notes"
            value={notes}
            onChange={(e) => setNotes(e.target.value)}
            placeholder="Any additional notes..."
            rows={3}
            className="input w-full"
          />
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
      </div>

      {showCreateZone && (
        <CreateZoneModal
          onClose={() => setShowCreateZone(false)}
          onCreated={handleZoneCreated}
        />
      )}
    </div>
  );
}

export default NodeNameEditor;
