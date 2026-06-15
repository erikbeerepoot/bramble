import { useState, useEffect, useCallback, useMemo } from 'react';
import { Plus, Pencil, Trash2, RefreshCw, X, Layers, Info } from 'lucide-react';
import type { Node, ValveGroup, ValveGroupMember } from '../types';
import {
  getValveGroups,
  createValveGroup,
  updateValveGroup,
  deleteValveGroup,
  resyncValveGroup,
} from '../api/client';
import { useAppContext } from '../App';

// Friendly name for a node, falling back to its hex device id (device ids are
// 64-bit strings — never coerce to number).
function nodeDisplayName(node: Node): string {
  return node.metadata?.name || `Node ${BigInt(node.device_id).toString(16).toUpperCase()}`;
}

// Display name for a device id that may or may not be a known node.
function deviceDisplayName(deviceId: string, nodeMap: Map<string, Node>): string {
  const node = nodeMap.get(deviceId);
  if (node) return nodeDisplayName(node);
  return `Node ${BigInt(deviceId).toString(16).toUpperCase()}`;
}

// Nodes that report a valve_count > 0 can host master/zone valves.
function hasValves(node: Node): boolean {
  return node.valve_count != null && node.valve_count > 0;
}

type ResyncStatus = { groupId: number; set: number; removed: number };

interface ValveGroupFormState {
  name: string;
  masterDeviceId: string;
  masterValve: number;
  members: ValveGroupMember[];
}

const EMPTY_FORM: ValveGroupFormState = {
  name: '',
  masterDeviceId: '',
  masterValve: 0,
  members: [],
};

function memberKey(member: ValveGroupMember): string {
  return `${member.zone_device_id}:${member.zone_valve}`;
}

function ValveGroups() {
  const { nodes } = useAppContext();

  const [groups, setGroups] = useState<ValveGroup[]>([]);
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState<string | null>(null);

  // Modal state: null = closed, otherwise the group being edited (or a fresh form for create).
  const [editing, setEditing] = useState<ValveGroup | 'new' | null>(null);

  const [deletingId, setDeletingId] = useState<number | null>(null);
  const [resyncingId, setResyncingId] = useState<number | null>(null);
  const [resyncStatus, setResyncStatus] = useState<ResyncStatus | null>(null);
  const [actionError, setActionError] = useState<string | null>(null);

  const valveNodes = useMemo(() => nodes.filter(hasValves), [nodes]);
  const nodeMap = useMemo(() => {
    const map = new Map<string, Node>();
    nodes.forEach((n) => map.set(n.device_id, n));
    return map;
  }, [nodes]);

  const fetchGroups = useCallback(async () => {
    try {
      setError(null);
      const response = await getValveGroups();
      setGroups(response.groups);
    } catch (err) {
      setError(err instanceof Error ? err.message : 'Failed to load valve groups');
    } finally {
      setLoading(false);
    }
  }, []);

  useEffect(() => {
    fetchGroups();
  }, [fetchGroups]);

  // Auto-clear the transient resync status after a few seconds.
  useEffect(() => {
    if (!resyncStatus) return;
    const timer = setTimeout(() => setResyncStatus(null), 5000);
    return () => clearTimeout(timer);
  }, [resyncStatus]);

  const handleDelete = async (group: ValveGroup) => {
    if (!window.confirm(`Delete valve group "${group.name}"? This removes the master valve link.`)) {
      return;
    }
    setDeletingId(group.id);
    setActionError(null);
    try {
      await deleteValveGroup(group.id);
      await fetchGroups();
    } catch (err) {
      setActionError(err instanceof Error ? err.message : 'Failed to delete valve group');
    } finally {
      setDeletingId(null);
    }
  };

  const handleResync = async (group: ValveGroup) => {
    setResyncingId(group.id);
    setActionError(null);
    setResyncStatus(null);
    try {
      const result = await resyncValveGroup(group.id);
      setResyncStatus({ groupId: group.id, set: result.set, removed: result.removed });
    } catch (err) {
      setActionError(err instanceof Error ? err.message : 'Failed to re-sync valve group');
    } finally {
      setResyncingId(null);
    }
  };

  const handleSaved = async () => {
    setEditing(null);
    await fetchGroups();
  };

  if (loading) {
    return (
      <div className="max-w-3xl">
        <h2 className="text-xl font-semibold text-gray-900 mb-6">Valve Groups</h2>
        <div className="space-y-3">
          {[0, 1].map((i) => (
            <div key={i} className="card animate-pulse">
              <div className="h-5 w-40 bg-gray-200 rounded mb-3" />
              <div className="h-4 w-64 bg-gray-200 rounded" />
            </div>
          ))}
        </div>
      </div>
    );
  }

  return (
    <div className="max-w-3xl">
      <div className="flex items-center justify-between mb-2">
        <h2 className="text-xl font-semibold text-gray-900">Valve Groups</h2>
        <button
          onClick={() => {
            setActionError(null);
            setEditing('new');
          }}
          className="btn btn-primary flex items-center gap-2"
        >
          <Plus className="w-4 h-4" />
          New Group
        </button>
      </div>

      <p className="text-sm text-gray-500 mb-6">
        A valve group pairs one <strong>master valve</strong> with one or more zone valves. The
        master opens automatically whenever any of its zone valves runs, acting as a series shutoff.
        The hub mirrors each zone&apos;s schedule onto the master node for you.
      </p>

      {error && (
        <div className="p-3 bg-red-50 border border-red-200 rounded-md text-red-700 text-sm mb-4">
          {error}
        </div>
      )}

      {actionError && (
        <div className="p-3 bg-red-50 border border-red-200 rounded-md text-red-700 text-sm mb-4 flex items-start justify-between gap-3">
          <span>{actionError}</span>
          <button
            onClick={() => setActionError(null)}
            className="text-red-400 hover:text-red-600"
            aria-label="Dismiss error"
          >
            <X className="w-4 h-4" />
          </button>
        </div>
      )}

      {groups.length === 0 ? (
        <div className="card text-center py-10">
          <Layers className="w-10 h-10 text-gray-300 mx-auto mb-3" />
          <p className="text-gray-600 font-medium">No valve groups yet</p>
          <p className="text-sm text-gray-500 mt-1">
            Create a group to link a master shutoff valve to its zone valves.
          </p>
        </div>
      ) : (
        <div className="space-y-3">
          {groups.map((group) => (
            <div key={group.id} className="card">
              <div className="flex items-start justify-between gap-3">
                <div className="min-w-0">
                  <h3 className="text-lg font-medium text-gray-900 truncate">{group.name}</h3>
                  <p className="text-sm text-gray-600 mt-0.5">
                    Master:{' '}
                    <span className="font-medium">
                      {deviceDisplayName(group.master_device_id, nodeMap)}
                    </span>{' '}
                    · Valve {group.master_valve + 1}
                  </p>
                </div>
                <div className="flex items-center gap-1 shrink-0">
                  <button
                    onClick={() => handleResync(group)}
                    disabled={resyncingId === group.id}
                    className="p-2 rounded-md text-gray-500 hover:bg-gray-100 hover:text-gray-700 disabled:opacity-50"
                    title="Re-sync master schedules to nodes"
                  >
                    <RefreshCw
                      className={`w-4 h-4 ${resyncingId === group.id ? 'animate-spin' : ''}`}
                    />
                  </button>
                  <button
                    onClick={() => {
                      setActionError(null);
                      setEditing(group);
                    }}
                    className="p-2 rounded-md text-gray-500 hover:bg-gray-100 hover:text-gray-700"
                    title="Edit group"
                  >
                    <Pencil className="w-4 h-4" />
                  </button>
                  <button
                    onClick={() => handleDelete(group)}
                    disabled={deletingId === group.id}
                    className="p-2 rounded-md text-red-500 hover:bg-red-50 hover:text-red-700 disabled:opacity-50"
                    title="Delete group"
                  >
                    <Trash2 className="w-4 h-4" />
                  </button>
                </div>
              </div>

              {/* Member chips */}
              <div className="mt-3">
                <p className="text-xs font-medium text-gray-500 mb-1.5">
                  {group.members.length} zone {group.members.length === 1 ? 'valve' : 'valves'}
                </p>
                <div className="flex flex-wrap gap-2">
                  {group.members.map((member) => (
                    <span
                      key={memberKey(member)}
                      className="inline-flex items-center gap-1 px-2.5 py-1 rounded-full bg-gray-100 text-gray-700 text-xs"
                    >
                      {deviceDisplayName(member.zone_device_id, nodeMap)} · V{member.zone_valve + 1}
                    </span>
                  ))}
                </div>
              </div>

              {/* Transient resync status */}
              {resyncStatus && resyncStatus.groupId === group.id && (
                <div className="mt-3 p-2.5 bg-green-50 border border-green-200 rounded-md text-green-700 text-sm flex items-center gap-2">
                  <RefreshCw className="w-4 h-4 shrink-0" />
                  <span>
                    Re-synced: {resyncStatus.set} master schedule
                    {resyncStatus.set === 1 ? '' : 's'} set, {resyncStatus.removed} removed.
                  </span>
                </div>
              )}
            </div>
          ))}
        </div>
      )}

      <div className="mt-6 flex items-start gap-2 text-xs text-gray-500">
        <Info className="w-4 h-4 shrink-0 mt-0.5" />
        <p>
          <strong>Re-sync</strong> pushes the master valve&apos;s mirrored schedules to its node
          again. Use it if a node was offline and missed an update.
        </p>
      </div>

      {editing && (
        <ValveGroupModal
          group={editing === 'new' ? null : editing}
          valveNodes={valveNodes}
          onClose={() => setEditing(null)}
          onSaved={handleSaved}
        />
      )}
    </div>
  );
}

interface ValveGroupModalProps {
  group: ValveGroup | null; // null = create
  valveNodes: Node[];
  onClose: () => void;
  onSaved: () => void;
}

function ValveGroupModal({ group, valveNodes, onClose, onSaved }: ValveGroupModalProps) {
  const isEdit = group !== null;

  const [form, setForm] = useState<ValveGroupFormState>(() =>
    group
      ? {
          name: group.name,
          masterDeviceId: group.master_device_id,
          masterValve: group.master_valve,
          members: group.members.map((m) => ({ ...m })),
        }
      : EMPTY_FORM
  );
  const [saving, setSaving] = useState(false);
  const [error, setError] = useState<string | null>(null);

  // Member-picker working state
  const [memberDeviceId, setMemberDeviceId] = useState<string>('');
  const [memberValve, setMemberValve] = useState<number>(0);

  const nodeMap = useMemo(() => {
    const map = new Map<string, Node>();
    valveNodes.forEach((n) => map.set(n.device_id, n));
    return map;
  }, [valveNodes]);

  const masterNode = form.masterDeviceId ? nodeMap.get(form.masterDeviceId) : undefined;
  const masterValveCount = masterNode?.valve_count ?? 0;

  const memberNode = memberDeviceId ? nodeMap.get(memberDeviceId) : undefined;
  const memberValveCount = memberNode?.valve_count ?? 0;

  // The master valve cannot also be a member. Build the set of valves already
  // claimed (as members) so we don't offer duplicates in the picker.
  const claimedMemberKeys = useMemo(
    () => new Set(form.members.map(memberKey)),
    [form.members]
  );

  const addMember = () => {
    if (!memberDeviceId) return;
    const candidate: ValveGroupMember = {
      zone_device_id: memberDeviceId,
      zone_valve: memberValve,
    };
    // Prevent selecting the master valve as a member.
    if (
      candidate.zone_device_id === form.masterDeviceId &&
      candidate.zone_valve === form.masterValve
    ) {
      setError('The master valve cannot also be a zone member.');
      return;
    }
    if (claimedMemberKeys.has(memberKey(candidate))) {
      setError('That valve is already a member of this group.');
      return;
    }
    setError(null);
    setForm((prev) => ({ ...prev, members: [...prev.members, candidate] }));
  };

  const removeMember = (member: ValveGroupMember) => {
    setForm((prev) => ({
      ...prev,
      members: prev.members.filter((m) => memberKey(m) !== memberKey(member)),
    }));
  };

  const handleSave = async () => {
    const name = form.name.trim();
    if (!name) {
      setError('Name is required.');
      return;
    }
    if (!form.masterDeviceId) {
      setError('Select a master node.');
      return;
    }
    if (form.members.length === 0) {
      setError('Add at least one zone valve.');
      return;
    }

    setSaving(true);
    setError(null);
    try {
      if (isEdit && group) {
        await updateValveGroup(group.id, {
          name,
          master_device_id: form.masterDeviceId,
          master_valve: form.masterValve,
          members: form.members,
        });
      } else {
        await createValveGroup({
          name,
          master_device_id: form.masterDeviceId,
          master_valve: form.masterValve,
          members: form.members,
        });
      }
      onSaved();
    } catch (err) {
      setError(err instanceof Error ? err.message : 'Failed to save valve group');
    } finally {
      setSaving(false);
    }
  };

  // When the master node changes, clamp the master valve to a valid index.
  const handleMasterNodeChange = (deviceId: string) => {
    setForm((prev) => ({ ...prev, masterDeviceId: deviceId, masterValve: 0 }));
  };

  // Valve options for a node with the given valve_count.
  const valveOptions = (count: number): number[] =>
    Array.from({ length: count }, (_, i) => i);

  return (
    <div className="fixed inset-0 z-50 flex items-center justify-center p-4">
      <div className="fixed inset-0 bg-black/40" onClick={onClose} />
      <div className="relative z-50 bg-white rounded-lg shadow-xl w-full max-w-lg max-h-[90vh] overflow-y-auto">
        <div className="px-6 py-4 border-b border-gray-200">
          <h3 className="text-lg font-medium text-gray-900">
            {isEdit ? 'Edit Valve Group' : 'Create Valve Group'}
          </h3>
        </div>

        <div className="px-6 py-4 space-y-5">
          {error && (
            <div className="p-3 bg-red-50 border border-red-200 rounded-md text-red-700 text-sm">
              {error}
            </div>
          )}

          {valveNodes.length === 0 && (
            <div className="p-3 bg-yellow-50 border border-yellow-200 rounded-md text-yellow-800 text-sm">
              No nodes report any valves. Connect an irrigation node before creating a group.
            </div>
          )}

          {/* Name */}
          <div>
            <label htmlFor="group-name" className="block text-sm font-medium text-gray-700 mb-1">
              Name *
            </label>
            <input
              id="group-name"
              type="text"
              value={form.name}
              onChange={(e) => setForm((prev) => ({ ...prev, name: e.target.value }))}
              placeholder="e.g., North field master"
              className="input w-full"
              autoFocus
            />
          </div>

          {/* Master valve */}
          <div>
            <label className="block text-sm font-medium text-gray-700 mb-1">Master valve *</label>
            <div className="flex gap-2">
              <select
                value={form.masterDeviceId}
                onChange={(e) => handleMasterNodeChange(e.target.value)}
                className="input flex-1"
              >
                <option value="">Select node…</option>
                {valveNodes.map((node) => (
                  <option key={node.device_id} value={node.device_id}>
                    {nodeDisplayName(node)}
                  </option>
                ))}
              </select>
              <select
                value={form.masterValve}
                onChange={(e) =>
                  setForm((prev) => ({ ...prev, masterValve: parseInt(e.target.value, 10) }))
                }
                disabled={!masterNode}
                className="input w-32 disabled:opacity-50"
              >
                {valveOptions(masterValveCount).map((v) => (
                  <option key={v} value={v}>
                    Valve {v + 1}
                  </option>
                ))}
              </select>
            </div>
            <p className="mt-1 text-xs text-gray-500">
              Opens automatically whenever any zone valve below runs.
            </p>
          </div>

          {/* Members */}
          <div>
            <label className="block text-sm font-medium text-gray-700 mb-2">Zone valves *</label>

            {form.members.length > 0 ? (
              <div className="flex flex-wrap gap-2 mb-3">
                {form.members.map((member) => (
                  <span
                    key={memberKey(member)}
                    className="inline-flex items-center gap-1.5 pl-2.5 pr-1.5 py-1 rounded-full bg-gray-100 text-gray-700 text-xs"
                  >
                    {deviceDisplayName(member.zone_device_id, nodeMap)} · V
                    {member.zone_valve + 1}
                    <button
                      type="button"
                      onClick={() => removeMember(member)}
                      className="text-gray-400 hover:text-gray-700"
                      aria-label="Remove zone valve"
                    >
                      <X className="w-3.5 h-3.5" />
                    </button>
                  </span>
                ))}
              </div>
            ) : (
              <p className="text-sm text-gray-500 mb-3">No zone valves added yet.</p>
            )}

            <div className="flex gap-2">
              <select
                value={memberDeviceId}
                onChange={(e) => {
                  setMemberDeviceId(e.target.value);
                  setMemberValve(0);
                }}
                className="input flex-1"
              >
                <option value="">Select node…</option>
                {valveNodes.map((node) => (
                  <option key={node.device_id} value={node.device_id}>
                    {nodeDisplayName(node)}
                  </option>
                ))}
              </select>
              <select
                value={memberValve}
                onChange={(e) => setMemberValve(parseInt(e.target.value, 10))}
                disabled={!memberNode}
                className="input w-28 disabled:opacity-50"
              >
                {valveOptions(memberValveCount).map((v) => (
                  <option key={v} value={v}>
                    Valve {v + 1}
                  </option>
                ))}
              </select>
              <button
                type="button"
                onClick={addMember}
                disabled={!memberDeviceId}
                className="btn btn-secondary disabled:opacity-50"
              >
                Add
              </button>
            </div>
          </div>
        </div>

        <div className="px-6 py-4 border-t border-gray-200 flex justify-end gap-3">
          <button onClick={onClose} disabled={saving} className="btn btn-secondary">
            Cancel
          </button>
          <button
            onClick={handleSave}
            disabled={saving || valveNodes.length === 0}
            className="btn btn-primary"
          >
            {saving ? 'Saving…' : isEdit ? 'Save Changes' : 'Create Group'}
          </button>
        </div>
      </div>
    </div>
  );
}

export default ValveGroups;
