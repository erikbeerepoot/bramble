import { useState, useMemo } from 'react';
import type { Node, Zone } from '../types';
import NodeCard from './NodeCard';

interface NodeListProps {
  nodes: Node[];
  zones: Zone[];
  onSelect: (node: Node) => void;
  onRefresh: () => void;
}

interface ZoneGroup {
  zone: Zone | null;
  nodes: Node[];
}

function NodeList({ nodes, zones, onSelect, onRefresh }: NodeListProps) {
  const [collapsedZones, setCollapsedZones] = useState<Set<number | 'unzoned'>>(new Set());

  const onlineNodes = nodes.filter(n => n.online);
  const offlineNodes = nodes.filter(n => !n.online);

  // Create a map of zone_id to Zone for quick lookup
  const zoneMap = useMemo(() => {
    const map = new Map<number, Zone>();
    zones.forEach(z => map.set(z.id, z));
    return map;
  }, [zones]);

  // Group nodes by zone
  const groupedNodes = useMemo(() => {
    const groups: ZoneGroup[] = [];
    const nodesByZone = new Map<number | null, Node[]>();

    // Group nodes by zone_id
    nodes.forEach(node => {
      const zoneId = node.metadata?.zone_id ?? null;
      if (!nodesByZone.has(zoneId)) {
        nodesByZone.set(zoneId, []);
      }
      nodesByZone.get(zoneId)!.push(node);
    });

    // Add zoned groups first (sorted by zone name)
    const zonedEntries: [number, Node[]][] = [];
    nodesByZone.forEach((nodeList, zoneId) => {
      if (zoneId !== null) {
        zonedEntries.push([zoneId, nodeList]);
      }
    });

    zonedEntries
      .sort((a, b) => {
        const zoneA = zoneMap.get(a[0]);
        const zoneB = zoneMap.get(b[0]);
        return (zoneA?.name || '').localeCompare(zoneB?.name || '');
      })
      .forEach(([zoneId, nodeList]) => {
        const zone = zoneMap.get(zoneId);
        if (zone) {
          groups.push({ zone, nodes: nodeList });
        }
      });

    // Add unzoned nodes at the end
    const unzonedNodes = nodesByZone.get(null);
    if (unzonedNodes && unzonedNodes.length > 0) {
      groups.push({ zone: null, nodes: unzonedNodes });
    }

    return groups;
  }, [nodes, zoneMap]);

  const toggleZone = (zoneKey: number | 'unzoned') => {
    setCollapsedZones(prev => {
      const next = new Set(prev);
      if (next.has(zoneKey)) {
        next.delete(zoneKey);
      } else {
        next.add(zoneKey);
      }
      return next;
    });
  };

  const getZoneKey = (zone: Zone | null): number | 'unzoned' => {
    return zone?.id ?? 'unzoned';
  };

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
        <div className="space-y-6">
          {groupedNodes.map((group) => {
            const zoneKey = getZoneKey(group.zone);
            const isCollapsed = collapsedZones.has(zoneKey);
            const onlineCount = group.nodes.filter(n => n.online).length;

            return (
              <div key={zoneKey}>
                <button
                  onClick={() => toggleZone(zoneKey)}
                  className="flex items-center space-x-2 mb-3 group"
                >
                  <svg
                    className={`w-4 h-4 text-gray-400 transition-transform ${isCollapsed ? '' : 'rotate-90'}`}
                    fill="none"
                    stroke="currentColor"
                    viewBox="0 0 24 24"
                  >
                    <path strokeLinecap="round" strokeLinejoin="round" strokeWidth={2} d="M9 5l7 7-7 7" />
                  </svg>
                  {group.zone ? (
                    <>
                      <span
                        className="w-3 h-3 rounded-full"
                        style={{ backgroundColor: group.zone.color }}
                      />
                      <span className="font-medium text-gray-900">{group.zone.name}</span>
                    </>
                  ) : (
                    <span className="font-medium text-gray-500">Unzoned</span>
                  )}
                  <span className="text-sm text-gray-400">
                    ({group.nodes.length} node{group.nodes.length !== 1 ? 's' : ''}, {onlineCount} online)
                  </span>
                </button>

                {!isCollapsed && (
                  <div className="grid grid-cols-1 md:grid-cols-2 lg:grid-cols-3 gap-4">
                    {group.nodes.map(node => (
                      <NodeCard
                        key={node.address}
                        node={node}
                        zone={group.zone ?? undefined}
                        onClick={() => onSelect(node)}
                      />
                    ))}
                  </div>
                )}
              </div>
            );
          })}
        </div>
      )}
    </div>
  );
}

export default NodeList;
