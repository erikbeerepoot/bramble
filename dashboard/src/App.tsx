import { useState, useEffect, useCallback, createContext, useContext } from 'react';
import { Link, Outlet, useLocation } from 'react-router-dom';
import type { Node, Zone } from './types';
import { getNodes, checkHealth, getZones } from './api/client';

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
    setNodes(prev => prev.map(n => n.address === updatedNode.address ? updatedNode : n));
  }, []);

  const addZone = useCallback((zone: Zone) => {
    setZones(prev => [...prev, zone].sort((a, b) => a.name.localeCompare(b.name)));
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
    }, 30000);

    return () => clearInterval(interval);
  }, [fetchNodes, fetchZones, checkConnection]);

  const isNodesActive = location.pathname === '/nodes' || location.pathname.startsWith('/nodes/');
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
    addZone,
  };

  return (
    <AppContext.Provider value={contextValue}>
      <div className="min-h-screen bg-gray-50">
        <header className="bg-bramble-700 text-white shadow-lg">
          <div className="max-w-7xl mx-auto px-4 py-4 sm:px-6 lg:px-8">
            <div className="flex items-center justify-between gap-4">
              <div className="flex items-center gap-3 min-w-0">
                <h1 className="text-xl sm:text-2xl font-bold truncate">Bramble</h1>
                <div className={`flex items-center space-x-2 text-sm shrink-0 ${connected ? 'text-green-300' : 'text-red-300'}`}>
                  <span className={`w-2 h-2 rounded-full ${connected ? 'bg-green-400' : 'bg-red-400'}`}></span>
                  <span className="hidden sm:inline">{connected ? 'Connected' : 'Disconnected'}</span>
                </div>
              </div>
              <nav className="flex space-x-2 sm:space-x-4 shrink-0">
                <Link
                  to="/nodes"
                  className={`px-3 py-2 rounded-md text-sm font-medium ${
                    isNodesActive ? 'bg-bramble-800' : 'hover:bg-bramble-600'
                  }`}
                >
                  Nodes
                </Link>
                <Link
                  to="/settings"
                  className={`px-3 py-2 rounded-md text-sm font-medium ${
                    isSettingsActive ? 'bg-bramble-800' : 'hover:bg-bramble-600'
                  }`}
                >
                  Settings
                </Link>
              </nav>
            </div>
          </div>
        </header>

        <main className="max-w-7xl mx-auto px-4 py-6 sm:px-6 lg:px-8">
          <Outlet />
        </main>
      </div>
    </AppContext.Provider>
  );
}

export default App;
