import { useState, useEffect, useCallback } from 'react';
import {
  runValve,
  stopValve,
  getIrrigationSchedules,
  addIrrigationSchedule,
  deleteIrrigationSchedule,
  getNodeEvents,
} from '../api/client';
import type { NodeEvent, IrrigationSchedule } from '../types';
import { getEventName, formatDays, formatDuration, DAY_LABELS } from '../types';

interface IrrigationControlProps {
  deviceId: string;
}

const DURATION_PRESETS = [
  { label: '5m', seconds: 300 },
  { label: '15m', seconds: 900 },
  { label: '30m', seconds: 1800 },
];

function IrrigationControl({ deviceId }: IrrigationControlProps) {
  // Run-once state
  const [selectedDuration, setSelectedDuration] = useState<Record<number, number>>({ 0: 300, 1: 300 });
  const [customMinutes, setCustomMinutes] = useState<Record<number, string>>({ 0: '', 1: '' });
  const [sendingValve, setSendingValve] = useState<number | null>(null);
  const [stoppingValve, setStoppingValve] = useState<number | null>(null);
  const [valveMessage, setValveMessage] = useState<string | null>(null);
  const [valveError, setValveError] = useState<string | null>(null);

  // Schedule state
  const [schedules, setSchedules] = useState<IrrigationSchedule[]>([]);
  const [loadingSchedules, setLoadingSchedules] = useState(true);
  const [showAddForm, setShowAddForm] = useState(false);
  const [scheduleError, setScheduleError] = useState<string | null>(null);
  const [deletingIndex, setDeletingIndex] = useState<number | null>(null);

  // Schedule form state
  const [formValve, setFormValve] = useState(0);
  const [formHour, setFormHour] = useState(6);
  const [formMinute, setFormMinute] = useState(0);
  const [formDurationMin, setFormDurationMin] = useState(15);
  const [formDays, setFormDays] = useState(127);
  const [savingSchedule, setSavingSchedule] = useState(false);

  // Events state
  const [events, setEvents] = useState<NodeEvent[]>([]);
  const [loadingEvents, setLoadingEvents] = useState(true);

  const fetchSchedules = useCallback(async () => {
    try {
      const response = await getIrrigationSchedules(deviceId);
      setSchedules(response.schedules);
    } catch {
      // Silently fail
    } finally {
      setLoadingSchedules(false);
    }
  }, [deviceId]);

  const fetchEvents = useCallback(async () => {
    try {
      const response = await getNodeEvents(deviceId, { limit: 20 });
      setEvents(response.events);
    } catch {
      // Silently fail
    } finally {
      setLoadingEvents(false);
    }
  }, [deviceId]);

  useEffect(() => {
    fetchSchedules();
    fetchEvents();
  }, [fetchSchedules, fetchEvents]);

  const handleRun = async (valve: number) => {
    setSendingValve(valve);
    setValveError(null);
    setValveMessage(null);
    try {
      const duration = selectedDuration[valve] || 300;
      await runValve(deviceId, valve, duration);
      setValveMessage(`Valve ${valve + 1} run command queued (${formatDuration(duration)})`);
      setTimeout(fetchEvents, 3000);
    } catch (err) {
      setValveError(err instanceof Error ? err.message : 'Failed to send command');
    } finally {
      setSendingValve(null);
    }
  };

  const handleStop = async (valve: number) => {
    setStoppingValve(valve);
    setValveError(null);
    setValveMessage(null);
    try {
      await stopValve(deviceId, valve);
      setValveMessage(`Valve ${valve + 1} stop command queued`);
      setTimeout(fetchEvents, 3000);
    } catch (err) {
      setValveError(err instanceof Error ? err.message : 'Failed to send stop command');
    } finally {
      setStoppingValve(null);
    }
  };

  const handleDurationSelect = (valve: number, seconds: number) => {
    setSelectedDuration((prev) => ({ ...prev, [valve]: seconds }));
    setCustomMinutes((prev) => ({ ...prev, [valve]: '' }));
  };

  const handleCustomMinutes = (valve: number, value: string) => {
    setCustomMinutes((prev) => ({ ...prev, [valve]: value }));
    const minutes = parseInt(value, 10);
    if (!isNaN(minutes) && minutes > 0 && minutes <= 120) {
      setSelectedDuration((prev) => ({ ...prev, [valve]: minutes * 60 }));
    }
  };

  const handleAddSchedule = async () => {
    setSavingSchedule(true);
    setScheduleError(null);
    try {
      // Find next available index
      const usedIndices = new Set(schedules.map((s) => s.index));
      let nextIndex = -1;
      for (let i = 0; i < 7; i++) {  // Reserve index 7 for run-once
        if (!usedIndices.has(i)) {
          nextIndex = i;
          break;
        }
      }
      if (nextIndex === -1) {
        setScheduleError('Maximum schedules reached (7)');
        setSavingSchedule(false);
        return;
      }

      await addIrrigationSchedule(deviceId, {
        index: nextIndex,
        hour: formHour,
        minute: formMinute,
        duration: formDurationMin * 60,
        days: formDays,
        valve: formValve,
      });

      setShowAddForm(false);
      setFormValve(0);
      setFormHour(6);
      setFormMinute(0);
      setFormDurationMin(15);
      setFormDays(127);
      await fetchSchedules();
    } catch (err) {
      setScheduleError(err instanceof Error ? err.message : 'Failed to add schedule');
    } finally {
      setSavingSchedule(false);
    }
  };

  const handleDeleteSchedule = async (index: number) => {
    setDeletingIndex(index);
    setScheduleError(null);
    try {
      await deleteIrrigationSchedule(deviceId, index);
      await fetchSchedules();
    } catch (err) {
      setScheduleError(err instanceof Error ? err.message : 'Failed to delete schedule');
    } finally {
      setDeletingIndex(null);
    }
  };

  const toggleDay = (bit: number) => {
    setFormDays((prev) => prev ^ (1 << bit));
  };

  const formatTime = (timestamp: number): string => {
    const date = new Date(timestamp * 1000);
    return date.toLocaleString(undefined, {
      month: 'short',
      day: 'numeric',
      hour: '2-digit',
      minute: '2-digit',
      second: '2-digit',
    });
  };

  const isBusy = sendingValve !== null || stoppingValve !== null;

  const renderValveRow = (valve: number) => (
    <div key={valve} className="space-y-2">
      <div className="flex items-center justify-between">
        <span className="text-sm font-medium text-gray-700">Valve {valve + 1}</span>
        <button
          onClick={() => handleStop(valve)}
          disabled={isBusy}
          className="text-xs px-2 py-1 rounded bg-red-100 text-red-700 hover:bg-red-200 disabled:opacity-50"
        >
          {stoppingValve === valve ? 'Stopping...' : 'Stop'}
        </button>
      </div>
      <div className="flex items-center gap-2">
        {DURATION_PRESETS.map((preset) => (
          <button
            key={preset.seconds}
            onClick={() => handleDurationSelect(valve, preset.seconds)}
            className={`px-3 py-1.5 text-sm rounded border ${
              selectedDuration[valve] === preset.seconds && !customMinutes[valve]
                ? 'bg-blue-50 border-blue-300 text-blue-700'
                : 'border-gray-200 text-gray-600 hover:border-gray-300'
            }`}
          >
            {preset.label}
          </button>
        ))}
        <input
          type="number"
          placeholder="min"
          min={1}
          max={120}
          value={customMinutes[valve]}
          onChange={(e) => handleCustomMinutes(valve, e.target.value)}
          className="w-16 px-2 py-1.5 text-sm border border-gray-200 rounded text-center"
        />
        <button
          onClick={() => handleRun(valve)}
          disabled={isBusy}
          className="px-4 py-1.5 text-sm rounded bg-green-600 text-white hover:bg-green-700 disabled:opacity-50"
        >
          {sendingValve === valve ? 'Sending...' : 'Run'}
        </button>
      </div>
    </div>
  );

  return (
    <div className="space-y-4">
      {/* Run Once */}
      <div className="card">
        <h3 className="text-lg font-medium text-gray-900 mb-4">Run Once</h3>
        <div className="space-y-4">
          {renderValveRow(0)}
          {renderValveRow(1)}
        </div>
        <p className="mt-3 text-xs text-gray-400">
          Best-effort — the node must be awake to receive the command.
        </p>
        {valveMessage && !valveError && (
          <p className="mt-2 text-sm text-green-600">{valveMessage}</p>
        )}
        {valveError && (
          <p className="mt-2 text-sm text-red-600">{valveError}</p>
        )}
      </div>

      {/* Schedules */}
      <div className="card">
        <h3 className="text-lg font-medium text-gray-900 mb-4">Schedules</h3>
        {loadingSchedules ? (
          <div className="animate-pulse space-y-2">
            <div className="h-4 bg-gray-200 rounded w-3/4" />
            <div className="h-4 bg-gray-200 rounded w-1/2" />
          </div>
        ) : schedules.length === 0 && !showAddForm ? (
          <p className="text-sm text-gray-400 mb-3">No schedules configured.</p>
        ) : (
          <div className="space-y-2 mb-3">
            {schedules.map((schedule) => (
              <div
                key={schedule.index}
                className="flex items-center justify-between p-3 bg-gray-50 rounded-lg"
              >
                <div>
                  <span className="text-sm font-medium text-gray-700">
                    Valve {schedule.valve + 1}
                  </span>
                  <span className="text-sm text-gray-500 mx-2">
                    {String(schedule.hour).padStart(2, '0')}:{String(schedule.minute).padStart(2, '0')}
                  </span>
                  <span className="text-sm text-gray-500">
                    {formatDuration(schedule.duration)}
                  </span>
                  <div className="text-xs text-gray-400 mt-0.5">
                    {formatDays(schedule.days)}
                  </div>
                </div>
                <button
                  onClick={() => handleDeleteSchedule(schedule.index)}
                  disabled={deletingIndex !== null}
                  className="text-sm text-red-500 hover:text-red-700 disabled:opacity-50"
                >
                  {deletingIndex === schedule.index ? 'Removing...' : 'Delete'}
                </button>
              </div>
            ))}
          </div>
        )}

        {showAddForm && (
          <div className="p-3 border border-gray-200 rounded-lg space-y-3 mb-3">
            <div className="grid grid-cols-2 gap-3">
              <div>
                <label className="block text-xs text-gray-500 mb-1">Valve</label>
                <select
                  value={formValve}
                  onChange={(e) => setFormValve(parseInt(e.target.value, 10))}
                  className="w-full px-2 py-1.5 text-sm border border-gray-200 rounded"
                >
                  <option value={0}>Valve 1</option>
                  <option value={1}>Valve 2</option>
                </select>
              </div>
              <div>
                <label className="block text-xs text-gray-500 mb-1">Duration (min)</label>
                <input
                  type="number"
                  min={1}
                  max={120}
                  value={formDurationMin}
                  onChange={(e) => setFormDurationMin(parseInt(e.target.value, 10) || 1)}
                  className="w-full px-2 py-1.5 text-sm border border-gray-200 rounded"
                />
              </div>
            </div>
            <div className="grid grid-cols-2 gap-3">
              <div>
                <label className="block text-xs text-gray-500 mb-1">Hour</label>
                <input
                  type="number"
                  min={0}
                  max={23}
                  value={formHour}
                  onChange={(e) => setFormHour(parseInt(e.target.value, 10) || 0)}
                  className="w-full px-2 py-1.5 text-sm border border-gray-200 rounded"
                />
              </div>
              <div>
                <label className="block text-xs text-gray-500 mb-1">Minute</label>
                <input
                  type="number"
                  min={0}
                  max={59}
                  value={formMinute}
                  onChange={(e) => setFormMinute(parseInt(e.target.value, 10) || 0)}
                  className="w-full px-2 py-1.5 text-sm border border-gray-200 rounded"
                />
              </div>
            </div>
            <div>
              <label className="block text-xs text-gray-500 mb-1">Days</label>
              <div className="flex gap-1">
                {DAY_LABELS.map((day, i) => (
                  <button
                    key={day}
                    onClick={() => toggleDay(i)}
                    className={`px-2 py-1 text-xs rounded ${
                      formDays & (1 << i)
                        ? 'bg-blue-100 text-blue-700 border border-blue-300'
                        : 'bg-gray-100 text-gray-400 border border-gray-200'
                    }`}
                  >
                    {day}
                  </button>
                ))}
              </div>
            </div>
            <div className="flex gap-2">
              <button
                onClick={handleAddSchedule}
                disabled={savingSchedule || formDays === 0}
                className="px-3 py-1.5 text-sm rounded bg-blue-600 text-white hover:bg-blue-700 disabled:opacity-50"
              >
                {savingSchedule ? 'Saving...' : 'Save'}
              </button>
              <button
                onClick={() => setShowAddForm(false)}
                className="px-3 py-1.5 text-sm rounded border border-gray-200 text-gray-600 hover:border-gray-300"
              >
                Cancel
              </button>
            </div>
          </div>
        )}

        {scheduleError && (
          <p className="text-sm text-red-600 mb-2">{scheduleError}</p>
        )}

        {!showAddForm && (
          <button
            onClick={() => setShowAddForm(true)}
            className="text-sm text-blue-600 hover:text-blue-700"
          >
            + Add Schedule
          </button>
        )}
      </div>

      {/* Recent Events */}
      <div className="card">
        <h4 className="text-sm font-medium text-gray-700 mb-3">Recent Events</h4>
        {loadingEvents ? (
          <div className="animate-pulse space-y-2">
            <div className="h-4 bg-gray-200 rounded w-3/4" />
            <div className="h-4 bg-gray-200 rounded w-1/2" />
          </div>
        ) : events.length === 0 ? (
          <p className="text-sm text-gray-400">No events recorded yet.</p>
        ) : (
          <div className="overflow-y-auto max-h-64">
            <table className="w-full text-sm">
              <tbody className="divide-y divide-gray-100">
                {events.map((event, index) => (
                  <tr key={index}>
                    <td className="py-1.5 text-gray-700">{getEventName(event.event_code)}</td>
                    <td className="py-1.5 text-gray-400 text-right whitespace-nowrap">{formatTime(event.timestamp)}</td>
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
        )}
      </div>
    </div>
  );
}

export default IrrigationControl;
