import { useState, useEffect, useCallback } from 'react';
import { Zap, Play, Square } from 'lucide-react';
import {
  runValve,
  stopValve,
  getIrrigationSchedules,
  addIrrigationSchedule,
  deleteIrrigationSchedule,
  getNodeEvents,
} from '../api/client';
import type { NodeEvent, IrrigationSchedule } from '../types';
import { formatDuration, DAY_LABELS } from '../types';
import { REFRESH_INTERVAL_MS } from '../config';
import { SchedulesList } from './SchedulesList';
import { RecentEvents } from './RecentEvents';

interface IrrigationControlProps {
  deviceId: string;
}

// Non-linear notches: fine-grained at short durations, coarser for long runs
const DURATION_NOTCHES = [1, 5, 10, 15, 20, 30, 45, 60, 90, 120]; // minutes

const DURATION_LABELS: Record<number, string> = {
  1: '1',
  5: '5',
  10: '10',
  15: '15',
  20: '20',
  30: '30',
  45: '45',
  60: '1h',
  90: '1.5h',
  120: '2h',
};

function formatDurationShort(minutes: number): string {
  if (minutes >= 60) {
    return minutes === 60 ? '1h' : minutes === 90 ? '1.5h' : '2h';
  }
  return `${minutes}m`;
}

function IrrigationControl({ deviceId }: IrrigationControlProps) {
  // Run-once state
  const [selectedDuration, setSelectedDuration] = useState<Record<number, number>>({
    0: 300,
    1: 300,
  });
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
    const interval = setInterval(() => {
      fetchSchedules();
      fetchEvents();
    }, REFRESH_INTERVAL_MS);
    return () => clearInterval(interval);
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

  const handleAddSchedule = async () => {
    setSavingSchedule(true);
    setScheduleError(null);
    try {
      // Find next available index
      const usedIndices = new Set(schedules.map((s) => s.index));
      let nextIndex = -1;
      for (let i = 0; i < 7; i++) {
        // Reserve index 7 for run-once
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

  const isBusy = sendingValve !== null || stoppingValve !== null;

  const renderValveRow = (valve: number) => {
    const duration = selectedDuration[valve] || DURATION_NOTCHES[0] * 60;
    const minutes = duration / 60;
    return (
      <div key={valve} className="space-y-3">
        {/* Header */}
        <h3 className="font-medium text-gray-900">Valve {valve + 1}</h3>

        {/* Duration chips */}
        <div className="overflow-x-auto scrollbar-hide">
          <div className="flex gap-2 min-w-min pb-1">
            {DURATION_NOTCHES.map((m) => (
              <button
                key={m}
                onClick={() => setSelectedDuration((prev) => ({ ...prev, [valve]: m * 60 }))}
                className={`flex-shrink-0 px-4 h-10 rounded-full text-sm font-medium transition-colors ${
                  m === minutes
                    ? 'bg-green-600 text-white'
                    : 'bg-gray-100 text-gray-700 hover:bg-gray-200 active:bg-gray-300'
                }`}
              >
                {DURATION_LABELS[m] || m}
              </button>
            ))}
          </div>
        </div>

        {/* Action buttons */}
        <div className="flex gap-3">
          <button
            onClick={() => handleRun(valve)}
            disabled={isBusy}
            className="flex-1 flex items-center justify-center gap-2 h-11 bg-green-600 text-white rounded-xl font-medium hover:bg-green-700 active:bg-green-800 transition-colors disabled:opacity-50"
          >
            <Play className="w-4 h-4 fill-current" />
            {sendingValve === valve ? 'Sending...' : `Run ${formatDurationShort(minutes)}`}
          </button>
          <button
            onClick={() => handleStop(valve)}
            disabled={isBusy}
            className="flex-1 flex items-center justify-center gap-2 h-11 bg-red-100 text-red-700 rounded-xl font-medium hover:bg-red-200 active:bg-red-300 transition-colors disabled:opacity-50"
          >
            <Square className="w-4 h-4 fill-current" />
            {stoppingValve === valve ? 'Stopping...' : 'Stop'}
          </button>
        </div>
      </div>
    );
  };

  return (
    <div className="space-y-4">
      {/* Run Once */}
      <div className="bg-white rounded-2xl shadow-md p-5">
        <div className="flex items-center gap-2 mb-5">
          <Zap className="w-5 h-5 text-gray-500" />
          <h2 className="font-semibold text-gray-900">Run Once</h2>
        </div>
        <div className="space-y-5">
          {renderValveRow(0)}
          <div className="border-t border-gray-200" />
          {renderValveRow(1)}
        </div>
        {valveMessage && !valveError && (
          <p className="mt-2 text-sm text-green-600">{valveMessage}</p>
        )}
        {valveError && <p className="mt-2 text-sm text-red-600">{valveError}</p>}
      </div>

      {/* Schedules */}
      <div>
        <SchedulesList
          schedules={schedules}
          loading={loadingSchedules}
          deletingIndex={deletingIndex}
          onDelete={handleDeleteSchedule}
          onAdd={() => setShowAddForm(true)}
        />
        {scheduleError && <p className="text-sm text-red-600 mt-2 px-1">{scheduleError}</p>}
      </div>

      {/* Add Schedule Modal */}
      {showAddForm && (
        <div className="fixed inset-0 z-40 flex items-center justify-center p-4">
          <div className="fixed inset-0 bg-black/40" onClick={() => setShowAddForm(false)} />
          <div className="relative z-50 bg-white rounded-2xl shadow-2xl w-full max-w-md max-h-[85vh] overflow-y-auto">
            <div className="px-6 pb-8 pt-6">
              <h2 className="text-xl font-semibold text-gray-900 mb-6 text-center">New Schedule</h2>

              {/* Valve Selector */}
              <div className="mb-6">
                <label className="block text-sm font-medium text-gray-700 mb-2">Valve</label>
                <div className="flex bg-gray-100 rounded-lg p-1">
                  {[0, 1].map((v) => (
                    <button
                      key={v}
                      onClick={() => setFormValve(v)}
                      className={`flex-1 py-2 px-4 rounded-md text-sm font-medium transition-all ${
                        formValve === v ? 'bg-white text-gray-900 shadow-sm' : 'text-gray-600'
                      }`}
                    >
                      Valve {v + 1}
                    </button>
                  ))}
                </div>
              </div>

              {/* Start Time — native input, renders as wheel on iOS */}
              <div className="mb-6">
                <label className="block text-sm font-medium text-gray-700 mb-2">Start Time</label>
                <input
                  type="time"
                  value={`${String(formHour).padStart(2, '0')}:${String(formMinute).padStart(2, '0')}`}
                  onChange={(e) => {
                    const [h, m] = e.target.value.split(':').map(Number);
                    setFormHour(h);
                    setFormMinute(m);
                  }}
                  className="w-full px-4 py-3 bg-gray-50 border border-gray-200 rounded-lg text-base focus:outline-none focus:ring-2 focus:ring-blue-500"
                />
              </div>

              {/* Duration */}
              <div className="mb-6">
                <label className="block text-sm font-medium text-gray-700 mb-2">Duration</label>
                <select
                  value={formDurationMin}
                  onChange={(e) => setFormDurationMin(parseInt(e.target.value, 10))}
                  className="w-full px-4 py-3 bg-gray-50 border border-gray-200 rounded-lg text-base focus:outline-none focus:ring-2 focus:ring-blue-500 appearance-none bg-[url('data:image/svg+xml;charset=utf-8,%3Csvg%20width%3D%2212%22%20height%3D%228%22%20viewBox%3D%220%200%2012%208%22%20fill%3D%22none%22%20xmlns%3D%22http%3A%2F%2Fwww.w3.org%2F2000%2Fsvg%22%3E%3Cpath%20d%3D%22M1%201.5L6%206.5L11%201.5%22%20stroke%3D%22%236B7280%22%20stroke-width%3D%221.5%22%20stroke-linecap%3D%22round%22%20stroke-linejoin%3D%22round%22%2F%3E%3C%2Fsvg%3E')] bg-no-repeat bg-[position:right_1rem_center]"
                >
                  {DURATION_NOTCHES.map((m) => (
                    <option key={m} value={m}>
                      {m >= 60
                        ? `${Math.floor(m / 60)}h${m % 60 > 0 ? ` ${m % 60}m` : ''}`
                        : `${m} minutes`}
                    </option>
                  ))}
                </select>
              </div>

              {/* Days */}
              <div className="mb-8">
                <label className="block text-sm font-medium text-gray-700 mb-3">Days</label>
                <div className="flex justify-between gap-2">
                  {DAY_LABELS.map((day, i) => {
                    const isSelected = (formDays & (1 << i)) !== 0;
                    return (
                      <button
                        key={day}
                        onClick={() => toggleDay(i)}
                        className={`w-11 h-11 rounded-full font-medium text-sm transition-all ${
                          isSelected
                            ? 'bg-blue-600 text-white'
                            : 'bg-white border-2 border-gray-300 text-gray-700 active:bg-gray-50'
                        }`}
                      >
                        {day.charAt(0)}
                      </button>
                    );
                  })}
                </div>
              </div>

              {/* Actions */}
              <div className="space-y-3">
                <button
                  onClick={handleAddSchedule}
                  disabled={savingSchedule || formDays === 0}
                  className="w-full py-4 bg-green-600 text-white font-semibold rounded-xl active:bg-green-700 disabled:opacity-50 transition-colors"
                >
                  {savingSchedule ? 'Saving...' : 'Save Schedule'}
                </button>
                <button
                  onClick={() => setShowAddForm(false)}
                  className="w-full py-2 text-gray-600 font-medium active:text-gray-800 transition-colors"
                >
                  Cancel
                </button>
              </div>
            </div>
          </div>
        </div>
      )}

      {/* Recent Events */}
      <RecentEvents events={events} loading={loadingEvents} />
    </div>
  );
}

export default IrrigationControl;
