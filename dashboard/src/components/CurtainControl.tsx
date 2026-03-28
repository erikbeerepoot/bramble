import { useState, useEffect, useCallback } from 'react';
import { controlCurtain, getNodeEvents } from '../api/client';
import type { NodeEvent } from '../types';
import { getEventName } from '../types';

interface CurtainControlProps {
  address: number;
  deviceId: string;
}

function CurtainControl({ address, deviceId }: CurtainControlProps) {
  const [sending, setSending] = useState<string | null>(null);
  const [lastAction, setLastAction] = useState<string | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [events, setEvents] = useState<NodeEvent[]>([]);
  const [loadingEvents, setLoadingEvents] = useState(true);
  const [calibrating, setCalibrating] = useState(false);

  const fetchEvents = useCallback(async () => {
    try {
      const response = await getNodeEvents(deviceId, { limit: 20 });
      setEvents(response.events);
    } catch {
      // Silently fail — events are informational
    } finally {
      setLoadingEvents(false);
    }
  }, [deviceId]);

  useEffect(() => {
    fetchEvents();
  }, [fetchEvents]);

  const handleAction = async (action: 'open' | 'close' | 'stop' | 'calibrate') => {
    setSending(action);
    setError(null);
    try {
      await controlCurtain(address, action);
      setLastAction(action);
      if (action === 'calibrate') {
        setCalibrating(true);
      } else if (action === 'stop' && calibrating) {
        setCalibrating(false);
      }
      // Refresh events after a short delay to allow the event to propagate
      setTimeout(fetchEvents, 3000);
    } catch (err) {
      setError(err instanceof Error ? err.message : 'Failed to send command');
    } finally {
      setSending(null);
    }
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

  return (
    <div className="card">
      <h3 className="text-lg font-medium text-gray-900 mb-4">Curtain Control</h3>

      <div className="flex gap-3">
        <button
          onClick={() => handleAction('open')}
          disabled={sending !== null}
          className="btn flex-1 bg-green-600 text-white hover:bg-green-700 disabled:opacity-50"
        >
          {sending === 'open' ? 'Sending...' : 'Open'}
        </button>
        <button
          onClick={() => handleAction('stop')}
          disabled={sending !== null}
          className="btn flex-1 bg-amber-500 text-white hover:bg-amber-600 disabled:opacity-50"
        >
          {sending === 'stop' ? 'Sending...' : 'Stop'}
        </button>
        <button
          onClick={() => handleAction('close')}
          disabled={sending !== null}
          className="btn flex-1 bg-red-600 text-white hover:bg-red-700 disabled:opacity-50"
        >
          {sending === 'close' ? 'Sending...' : 'Close'}
        </button>
      </div>

      {lastAction && !error && (
        <p className="mt-3 text-sm text-green-600">
          Curtain {lastAction} command queued.
        </p>
      )}
      {error && (
        <p className="mt-3 text-sm text-red-600">{error}</p>
      )}

      {/* Calibration */}
      <div className="mt-4 border-t border-gray-200 pt-4">
        {calibrating ? (
          <div className="space-y-2">
            <p className="text-sm text-amber-600 font-medium">
              Calibrating... curtain will close fully, pause, then open.
            </p>
            <p className="text-sm text-gray-500">
              Click <strong>Stop</strong> when the curtain is fully open to record the travel time.
            </p>
          </div>
        ) : (
          <button
            onClick={() => handleAction('calibrate')}
            disabled={sending !== null}
            className="btn w-full bg-gray-100 text-gray-700 hover:bg-gray-200 disabled:opacity-50"
          >
            {sending === 'calibrate' ? 'Starting...' : 'Calibrate Travel Time'}
          </button>
        )}
      </div>

      {/* Event History */}
      <div className="mt-4 border-t border-gray-200 pt-4">
        <h4 className="text-sm font-medium text-gray-700 mb-2">Recent Events</h4>
        {loadingEvents ? (
          <div className="animate-pulse space-y-2">
            <div className="h-4 bg-gray-200 rounded w-3/4" />
            <div className="h-4 bg-gray-200 rounded w-1/2" />
          </div>
        ) : events.length === 0 ? (
          <p className="text-sm text-gray-400">No events recorded yet.</p>
        ) : (
          <ul className="space-y-1">
            {events.map((event, index) => (
              <li key={index} className="flex justify-between text-sm">
                <span className="text-gray-700">{getEventName(event.event_code)}</span>
                <span className="text-gray-400">{formatTime(event.timestamp)}</span>
              </li>
            ))}
          </ul>
        )}
      </div>
    </div>
  );
}

export default CurtainControl;
