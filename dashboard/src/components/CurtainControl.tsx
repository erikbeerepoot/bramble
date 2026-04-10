import { useState } from 'react';
import { controlCurtain } from '../api/client';

interface CurtainControlProps {
  address: number;
  onEventTriggered?: () => void;
}

function CurtainControl({ address, onEventTriggered }: CurtainControlProps) {
  const [sending, setSending] = useState<string | null>(null);
  const [lastAction, setLastAction] = useState<string | null>(null);
  const [error, setError] = useState<string | null>(null);

  const handleAction = async (action: 'open' | 'close' | 'stop') => {
    setSending(action);
    setError(null);
    try {
      await controlCurtain(address, action);
      setLastAction(action);
      // Refresh events after a short delay to allow the event to propagate
      if (onEventTriggered) setTimeout(onEventTriggered, 3000);
    } catch (err) {
      setError(err instanceof Error ? err.message : 'Failed to send command');
    } finally {
      setSending(null);
    }
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
        <p className="mt-3 text-sm text-green-600">Curtain {lastAction} command queued.</p>
      )}
      {error && <p className="mt-3 text-sm text-red-600">{error}</p>}
    </div>
  );
}

export default CurtainControl;
