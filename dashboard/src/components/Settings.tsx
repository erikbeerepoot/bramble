import { useState } from 'react';
import { getApiUrl, setApiUrl, checkHealth } from '../api/client';

interface SettingsProps {
  onSave: () => void;
}

function Settings({ onSave }: SettingsProps) {
  const [url, setUrl] = useState(getApiUrl());
  const [testing, setTesting] = useState(false);
  const [testResult, setTestResult] = useState<{ success: boolean; message: string } | null>(null);

  const handleTest = async () => {
    setTesting(true);
    setTestResult(null);

    // Temporarily set the URL for testing
    const originalUrl = getApiUrl();
    setApiUrl(url);

    try {
      const health = await checkHealth();
      setTestResult({
        success: true,
        message: `Connected! Status: ${health.status}, Serial: ${health.serial_connected ? 'connected' : 'disconnected'}`,
      });
    } catch (err) {
      setTestResult({
        success: false,
        message: err instanceof Error ? err.message : 'Connection failed',
      });
      // Restore original URL on failure
      setApiUrl(originalUrl);
    } finally {
      setTesting(false);
    }
  };

  const handleSave = () => {
    setApiUrl(url);
    onSave();
  };

  return (
    <div className="max-w-lg">
      <h2 className="text-xl font-semibold text-gray-900 mb-6">Settings</h2>

      <div className="card">
        <h3 className="text-lg font-medium text-gray-900 mb-4">API Connection</h3>

        <div className="space-y-4">
          <div>
            <label htmlFor="apiUrl" className="block text-sm font-medium text-gray-700 mb-1">
              API URL
            </label>
            <input
              id="apiUrl"
              type="url"
              value={url}
              onChange={(e) => setUrl(e.target.value)}
              placeholder="http://pi:5000"
              className="input w-full"
            />
            <p className="mt-1 text-sm text-gray-500">
              The URL of your Bramble API server (usually running on your Raspberry Pi)
            </p>
          </div>

          {testResult && (
            <div className={`p-3 rounded-md text-sm ${
              testResult.success
                ? 'bg-green-50 border border-green-200 text-green-700'
                : 'bg-red-50 border border-red-200 text-red-700'
            }`}>
              {testResult.message}
            </div>
          )}

          <div className="flex space-x-3">
            <button
              onClick={handleTest}
              disabled={testing || !url}
              className="btn btn-secondary"
            >
              {testing ? 'Testing...' : 'Test Connection'}
            </button>
            <button
              onClick={handleSave}
              disabled={!url}
              className="btn btn-primary"
            >
              Save & Return
            </button>
          </div>
        </div>
      </div>

      <div className="card mt-4">
        <h3 className="text-lg font-medium text-gray-900 mb-2">About</h3>
        <p className="text-sm text-gray-600">
          Bramble Dashboard v0.1.0
        </p>
        <p className="text-sm text-gray-500 mt-1">
          Monitor your LoRa sensor network and irrigation system.
        </p>
      </div>
    </div>
  );
}

export default Settings;
