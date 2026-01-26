import { useState, useEffect } from 'react';
import { useParams, useNavigate } from 'react-router-dom';
import type { Node } from '../types';
import { useAppContext } from '../App';
import { getNode } from '../api/client';
import NodeDetail from './NodeDetail';

function NodeDetailPage() {
  const { address } = useParams<{ address: string }>();
  const navigate = useNavigate();
  const { nodes, zones, updateNode, addZone } = useAppContext();
  const [node, setNode] = useState<Node | null>(null);
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState<string | null>(null);

  useEffect(() => {
    async function loadNode() {
      if (!address) {
        setError('No node address provided');
        setLoading(false);
        return;
      }

      const addressNum = parseInt(address, 10);
      if (isNaN(addressNum)) {
        setError('Invalid node address');
        setLoading(false);
        return;
      }

      // First try to find the node in the cached list
      const cachedNode = nodes.find((n) => n.address === addressNum);
      if (cachedNode) {
        setNode(cachedNode);
        setLoading(false);
        return;
      }

      // If not in cache, fetch from API
      try {
        const fetchedNode = await getNode(addressNum);
        setNode(fetchedNode);
      } catch (err) {
        setError(err instanceof Error ? err.message : 'Failed to load node');
      } finally {
        setLoading(false);
      }
    }

    loadNode();
  }, [address, nodes]);

  // Update local node state when it changes in the nodes list
  useEffect(() => {
    if (node && address) {
      const addressNum = parseInt(address, 10);
      const updatedNode = nodes.find((n) => n.address === addressNum);
      if (updatedNode) {
        setNode(updatedNode);
      }
    }
  }, [nodes, address, node]);

  const handleBack = () => {
    navigate('/nodes');
  };

  const handleUpdate = (updatedNode: Node) => {
    setNode(updatedNode);
    updateNode(updatedNode);
  };

  if (loading) {
    return (
      <div className="text-center py-12">
        <div className="inline-block animate-spin rounded-full h-8 w-8 border-4 border-bramble-600 border-t-transparent"></div>
        <p className="mt-2 text-gray-600 dark:text-gray-400">Loading node...</p>
      </div>
    );
  }

  if (error || !node) {
    return (
      <div className="card bg-red-50 dark:bg-red-900/20 border border-red-200 dark:border-red-800">
        <p className="text-red-700 dark:text-red-400">Error: {error || 'Node not found'}</p>
        <button onClick={handleBack} className="mt-2 btn btn-primary">
          Back to Nodes
        </button>
      </div>
    );
  }

  return (
    <NodeDetail
      node={node}
      zones={zones}
      onBack={handleBack}
      onUpdate={handleUpdate}
      onZoneCreated={addZone}
    />
  );
}

export default NodeDetailPage;
