import { useState, useCallback } from 'react';
import { Zap, ChevronUp, ChevronDown, Square, Check, X } from 'lucide-react';
import { motion, AnimatePresence } from 'motion/react';

export type CurtainAction = 'open' | 'close' | 'stop';
type CurtainActionState = 'idle' | 'sending' | 'queued' | 'error';

interface ButtonState {
  state: CurtainActionState;
  action?: CurtainAction;
  errorMessage?: string;
}

interface CurtainControlProps {
  address: number;
  issueCurtain: (action: CurtainAction, address: number) => Promise<void>;
}

function CurtainControl({ address, issueCurtain }: CurtainControlProps) {
  const [buttonState, setButtonState] = useState<ButtonState>({ state: 'idle' });

  const dismissError = useCallback(() => {
    setButtonState({ state: 'idle' });
  }, []);

  const handleAction = async (action: CurtainAction) => {
    setButtonState({ state: 'sending', action });
    try {
      await issueCurtain(action, address);
      setButtonState({ state: 'queued', action });
      setTimeout(() => {
        setButtonState((prev) => (prev.state === 'queued' ? { state: 'idle' } : prev));
      }, 2000);
    } catch (err) {
      setButtonState({
        state: 'error',
        action,
        errorMessage: err instanceof Error ? err.message : 'Failed to send command',
      });
    }
  };

  const isBusy = buttonState.state === 'sending' || buttonState.state === 'queued';
  const isSending = (action: CurtainAction) =>
    buttonState.state === 'sending' && buttonState.action === action;
  const isQueued = (action: CurtainAction) =>
    buttonState.state === 'queued' && buttonState.action === action;

  const renderActionButton = (
    action: CurtainAction,
    label: string,
    Icon: typeof ChevronUp,
    baseClass: string
  ) => (
    <motion.button
      onClick={() => handleAction(action)}
      disabled={isBusy}
      className={`relative flex-1 h-11 rounded-xl font-medium transition-colors disabled:cursor-not-allowed disabled:opacity-90 ${baseClass}`}
      animate={{ scale: isQueued(action) ? [1, 1.03, 1] : 1 }}
      transition={{ duration: 0.3 }}
    >
      <AnimatePresence mode="wait" initial={false}>
        {isSending(action) ? (
          <motion.div
            key="sending"
            initial={{ opacity: 0, y: 8 }}
            animate={{ opacity: 1, y: 0 }}
            exit={{ opacity: 0, y: -8 }}
            transition={{ duration: 0.18 }}
            className="flex items-center justify-center gap-2"
          >
            <motion.div
              animate={{ rotate: 360 }}
              transition={{ duration: 1, repeat: Infinity, ease: 'linear' }}
            >
              <Zap className="w-4 h-4" />
            </motion.div>
            <span>Sending...</span>
          </motion.div>
        ) : isQueued(action) ? (
          <motion.div
            key="queued"
            initial={{ opacity: 0, scale: 0.85 }}
            animate={{ opacity: 1, scale: 1 }}
            exit={{ opacity: 0, scale: 0.85 }}
            transition={{ duration: 0.25, type: 'spring' }}
            className="flex items-center justify-center gap-2"
          >
            <Check className="w-5 h-5 stroke-[3]" />
            <span>Queued</span>
          </motion.div>
        ) : (
          <motion.div
            key="idle"
            initial={{ opacity: 0, y: 8 }}
            animate={{ opacity: 1, y: 0 }}
            exit={{ opacity: 0, y: -8 }}
            transition={{ duration: 0.18 }}
            className="flex items-center justify-center gap-2"
          >
            <Icon className="w-4 h-4" />
            <span>{label}</span>
          </motion.div>
        )}
      </AnimatePresence>
    </motion.button>
  );

  return (
    <div className="card">
      <h3 className="text-lg font-medium text-gray-900 mb-4">Curtain Control</h3>

      <div className="flex gap-3">
        {renderActionButton(
          'open',
          'Open',
          ChevronUp,
          'bg-green-600 text-white hover:bg-green-700 active:bg-green-800'
        )}
        {renderActionButton(
          'stop',
          'Stop',
          Square,
          'bg-amber-500 text-white hover:bg-amber-600 active:bg-amber-700'
        )}
        {renderActionButton(
          'close',
          'Close',
          ChevronDown,
          'bg-red-600 text-white hover:bg-red-700 active:bg-red-800'
        )}
      </div>

      {/* Persistent error — stays until dismissed */}
      <AnimatePresence>
        {buttonState.state === 'error' && buttonState.errorMessage && (
          <motion.div
            initial={{ opacity: 0, height: 0 }}
            animate={{ opacity: 1, height: 'auto' }}
            exit={{ opacity: 0, height: 0 }}
            transition={{ duration: 0.2 }}
            className="mt-3 bg-red-50 border border-red-200 rounded-xl p-3 flex items-start justify-between gap-3 overflow-hidden"
          >
            <div className="flex items-start gap-2 flex-1 min-w-0">
              <X className="w-4 h-4 text-red-600 mt-0.5 flex-shrink-0" />
              <p className="text-sm text-red-700 break-words">{buttonState.errorMessage}</p>
            </div>
            <button
              onClick={dismissError}
              className="text-red-400 hover:text-red-600 transition-colors flex-shrink-0"
              aria-label="Dismiss error"
            >
              <X className="w-4 h-4" />
            </button>
          </motion.div>
        )}
      </AnimatePresence>
    </div>
  );
}

export default CurtainControl;
