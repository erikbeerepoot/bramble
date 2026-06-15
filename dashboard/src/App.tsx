import { useState, useEffect, useCallback, createContext, useContext } from 'react';
import { Link, Outlet, useLocation } from 'react-router-dom';
import type { Node, Zone } from './types';
import { getNodes, checkHealth, getZones } from './api/client';
import { REFRESH_INTERVAL_MS } from './config';

interface AppContextType {
  nodes: Node[];
  zones: Zone[];
  loading: boolean;
  refreshing: boolean;
  error: string | null;
  connected: boolean;
  fetchNodes: () => Promise<void>;
  fetchZones: () => Promise<void>;
  updateNode: (node: Node) => void;
  removeNode: (deviceId: string) => void;
  addZone: (zone: Zone) => void;
}

const AppContext = createContext<AppContextType | null>(null);

export function useAppContext() {
  const context = useContext(AppContext);
  if (!context) {
    throw new Error('useAppContext must be used within AppProvider');
  }
  return context;
}

function App() {
  const [nodes, setNodes] = useState<Node[]>([]);
  const [zones, setZones] = useState<Zone[]>([]);
  const [loading, setLoading] = useState(true);
  const [refreshing, setRefreshing] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [connected, setConnected] = useState(false);
  const location = useLocation();

  const fetchNodes = useCallback(async () => {
    setRefreshing(true);
    try {
      setError(null);
      const response = await getNodes();
      setNodes(response.nodes);
      setConnected(true);
    } catch (err) {
      setError(err instanceof Error ? err.message : 'Failed to fetch nodes');
      setConnected(false);
    } finally {
      setLoading(false);
      setRefreshing(false);
    }
  }, []);

  const fetchZones = useCallback(async () => {
    try {
      const response = await getZones();
      setZones(response.zones);
    } catch (err) {
      console.error('Failed to fetch zones:', err);
    }
  }, []);

  const updateNode = useCallback((updatedNode: Node) => {
    setNodes((prev) => prev.map((n) => (n.device_id === updatedNode.device_id ? updatedNode : n)));
  }, []);

  const removeNode = useCallback((deviceId: string) => {
    setNodes((prev) => prev.filter((n) => n.device_id !== deviceId));
  }, []);

  const addZone = useCallback((zone: Zone) => {
    setZones((prev) => [...prev, zone].sort((a, b) => a.name.localeCompare(b.name)));
  }, []);

  const checkConnection = useCallback(async () => {
    try {
      const health = await checkHealth();
      setConnected(health.status === 'healthy');
    } catch {
      setConnected(false);
    }
  }, []);

  useEffect(() => {
    fetchNodes();
    fetchZones();
    checkConnection();

    const interval = setInterval(() => {
      fetchNodes();
    }, REFRESH_INTERVAL_MS);

    return () => clearInterval(interval);
  }, [fetchNodes, fetchZones, checkConnection]);

  const isNodesActive = location.pathname === '/nodes' || location.pathname.startsWith('/nodes/');
  const isVisualizeActive = location.pathname === '/visualize';
  const isValveGroupsActive = location.pathname === '/valve-groups';
  const isSettingsActive = location.pathname === '/settings';

  const contextValue: AppContextType = {
    nodes,
    zones,
    loading,
    refreshing,
    error,
    connected,
    fetchNodes,
    fetchZones,
    updateNode,
    removeNode,
    addZone,
  };

  return (
    <AppContext.Provider value={contextValue}>
      <div className="min-h-screen bg-surface">
        <header className="bg-brand text-white shadow-md">
          <div className="px-6 py-4">
            <div className="flex items-center justify-between gap-4">
              <div className="flex items-center gap-3 min-w-0">
                <h1 className="text-xl font-semibold truncate">Bramble</h1>
                <div className="flex items-center gap-2 shrink-0">
                  <div
                    className={`w-2 h-2 rounded-full ${connected ? 'bg-status-online shadow-sm shadow-green-400' : 'bg-status-error shadow-sm shadow-red-400'}`}
                  ></div>
                  <span
                    className={`text-sm hidden sm:inline ${connected ? 'opacity-90' : 'text-red-300'}`}
                  >
                    {connected ? 'Connected' : 'Disconnected'}
                  </span>
                </div>
              </div>
              <nav className="flex gap-2 sm:gap-4 shrink-0">
                <Link
                  to="/nodes"
                  className={`px-3 py-1.5 rounded text-sm font-medium transition-opacity ${
                    isNodesActive ? 'bg-white/15' : 'hover:bg-white/10 hover:opacity-80'
                  }`}
                >
                  Nodes
                </Link>
                <Link
                  to="/visualize"
                  className={`px-3 py-1.5 rounded text-sm font-medium transition-opacity ${
                    isVisualizeActive ? 'bg-white/15' : 'hover:bg-white/10 hover:opacity-80'
                  }`}
                >
                  Visualize
                </Link>
                <Link
                  to="/valve-groups"
                  className={`px-3 py-1.5 rounded text-sm font-medium transition-opacity ${
                    isValveGroupsActive ? 'bg-white/15' : 'hover:bg-white/10 hover:opacity-80'
                  }`}
                >
                  Valve Groups
                </Link>
                <Link
                  to="/settings"
                  className={`px-3 py-1.5 rounded text-sm font-medium transition-opacity ${
                    isSettingsActive ? 'bg-white/15' : 'hover:bg-white/10 hover:opacity-80'
                  }`}
                >
                  Settings
                </Link>
              </nav>
            </div>
          </div>
        </header>

        <main className="max-w-7xl mx-auto px-6 py-8">
          <Outlet />
        </main>
      </div>
    </AppContext.Provider>
  );
}

export default App;
