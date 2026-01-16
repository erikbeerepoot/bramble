import type { Node } from '../types';
import NodeCard from './NodeCard';

interface NodeListProps {
  nodes: Node[];
  onSelect: (node: Node) => void;
  onRefresh: () => void;
}

function NodeList({ nodes, onSelect, onRefresh }: NodeListProps) {
  const onlineNodes = nodes.filter(n => n.online);
  const offlineNodes = nodes.filter(n => !n.online);

  return (
    <div>
      <div className="flex items-center justify-between mb-6">
        <div>
          <h2 className="text-xl font-semibold text-gray-900">Network Nodes</h2>
          <p className="text-sm text-gray-500">
            {onlineNodes.length} online, {offlineNodes.length} offline
          </p>
        </div>
        <button
          onClick={onRefresh}
          className="btn btn-secondary flex items-center space-x-2"
        >
          <svg className="w-4 h-4" fill="none" stroke="currentColor" viewBox="0 0 24 24">
            <path strokeLinecap="round" strokeLinejoin="round" strokeWidth={2} d="M4 4v5h.582m15.356 2A8.001 8.001 0 004.582 9m0 0H9m11 11v-5h-.581m0 0a8.003 8.003 0 01-15.357-2m15.357 2H15" />
          </svg>
          <span>Refresh</span>
        </button>
      </div>

      {nodes.length === 0 ? (
        <div className="card text-center py-12">
          <svg className="mx-auto h-12 w-12 text-gray-400" fill="none" stroke="currentColor" viewBox="0 0 24 24">
            <path strokeLinecap="round" strokeLinejoin="round" strokeWidth={2} d="M9.75 17L9 20l-1 1h8l-1-1-.75-3M3 13h18M5 17h14a2 2 0 002-2V5a2 2 0 00-2-2H5a2 2 0 00-2 2v10a2 2 0 002 2z" />
          </svg>
          <h3 className="mt-2 text-sm font-medium text-gray-900">No nodes found</h3>
          <p className="mt-1 text-sm text-gray-500">
            No nodes are currently registered with the hub.
          </p>
        </div>
      ) : (
        <div className="grid grid-cols-1 md:grid-cols-2 lg:grid-cols-3 gap-4">
          {nodes.map(node => (
            <NodeCard
              key={node.address}
              node={node}
              onClick={() => onSelect(node)}
            />
          ))}
        </div>
      )}
    </div>
  );
}

export default NodeList;
