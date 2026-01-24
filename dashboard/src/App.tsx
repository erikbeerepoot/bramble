import { useState, useEffect, useCallback } from 'react';
import type { Node, Zone } from './types';
import { getNodes, checkHealth, getZones } from './api/client';
import NodeList from './components/NodeList';
import NodeDetail from './components/NodeDetail';
import Settings from './components/Settings';

type View = 'nodes' | 'settings';

function App() {
  const [nodes, setNodes] = useState<Node[]>([]);
  const [zones, setZones] = useState<Zone[]>([]);
  const [selectedNode, setSelectedNode] = useState<Node | null>(null);
  const [loading, setLoading] = useState(true);
  const [refreshing, setRefreshing] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [view, setView] = useState<View>('nodes');
  const [connected, setConnected] = useState(false);

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

  const handleZoneCreated = (zone: Zone) => {
    setZones(prev => [...prev, zone].sort((a, b) => a.name.localeCompare(b.name)));
  };

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

  const handleNodeSelect = (node: Node) => {
    setSelectedNode(node);
  };

  const handleBackToList = () => {
    setSelectedNode(null);
  };

  const handleNodeUpdate = (updatedNode: Node) => {
    setNodes(nodes.map(n => n.address === updatedNode.address ? updatedNode : n));
    if (selectedNode?.address === updatedNode.address) {
      setSelectedNode(updatedNode);
    }
  };

  return (
    <div className="min-h-screen bg-gray-50">
      <header className="bg-bramble-700 text-white shadow-lg">
        <div className="max-w-7xl mx-auto px-4 py-4 sm:px-6 lg:px-8">
          <div className="flex items-center justify-between">
            <div className="flex items-center space-x-4">
              <h1 className="text-2xl font-bold">Bramble Dashboard</h1>
              <div className={`flex items-center space-x-2 text-sm ${connected ? 'text-green-300' : 'text-red-300'}`}>
                <span className={`w-2 h-2 rounded-full ${connected ? 'bg-green-400' : 'bg-red-400'}`}></span>
                <span>{connected ? 'Connected' : 'Disconnected'}</span>
              </div>
            </div>
            <nav className="flex space-x-4">
              <button
                onClick={() => { setView('nodes'); setSelectedNode(null); }}
                className={`px-3 py-2 rounded-md text-sm font-medium ${
                  view === 'nodes' ? 'bg-bramble-800' : 'hover:bg-bramble-600'
                }`}
              >
                Nodes
              </button>
              <button
                onClick={() => setView('settings')}
                className={`px-3 py-2 rounded-md text-sm font-medium ${
                  view === 'settings' ? 'bg-bramble-800' : 'hover:bg-bramble-600'
                }`}
              >
                Settings
              </button>
            </nav>
          </div>
        </div>
      </header>

      <main className="max-w-7xl mx-auto px-4 py-6 sm:px-6 lg:px-8">
        {view === 'settings' ? (
          <Settings onSave={() => { fetchNodes(); setView('nodes'); }} />
        ) : selectedNode ? (
          <NodeDetail
            node={selectedNode}
            zones={zones}
            onBack={handleBackToList}
            onUpdate={handleNodeUpdate}
            onZoneCreated={handleZoneCreated}
          />
        ) : (
          <>
            {loading ? (
              <div className="text-center py-12">
                <div className="inline-block animate-spin rounded-full h-8 w-8 border-4 border-bramble-600 border-t-transparent"></div>
                <p className="mt-2 text-gray-600">Loading nodes...</p>
              </div>
            ) : error ? (
              <div className="card bg-red-50 border border-red-200">
                <p className="text-red-700">Error: {error}</p>
                <button
                  onClick={fetchNodes}
                  className="mt-2 btn btn-primary"
                >
                  Retry
                </button>
              </div>
            ) : (
              <NodeList
                nodes={nodes}
                zones={zones}
                onSelect={handleNodeSelect}
                onRefresh={fetchNodes}
                refreshing={refreshing}
              />
            )}
          </>
        )}
      </main>
    </div>
  );
}

export default App;
