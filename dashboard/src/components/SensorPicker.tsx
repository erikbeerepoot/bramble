import type { Node } from '../types';

interface SensorPickerProps {
  nodes: Node[];
  selectedIds: string[];
  onChange: (ids: string[]) => void;
  colors: string[];
}

function SensorPicker({ nodes, selectedIds, onChange, colors }: SensorPickerProps) {
  const sensorNodes = nodes.filter((n) => n.type === 'SENSOR');

  const toggleNode = (deviceId: string) => {
    if (selectedIds.includes(deviceId)) {
      onChange(selectedIds.filter((id) => id !== deviceId));
    } else {
      onChange([...selectedIds, deviceId]);
    }
  };

  const selectAll = () => {
    onChange(sensorNodes.map((n) => n.device_id));
  };

  const clearAll = () => {
    onChange([]);
  };

  if (sensorNodes.length === 0) {
    return <p className="text-sm text-gray-500">No sensor nodes available.</p>;
  }

  return (
    <div className="space-y-2">
      <div className="flex items-center gap-2">
        <span className="text-sm font-medium text-gray-700">Sensors:</span>
        <button onClick={selectAll} className="text-xs text-bramble-600 hover:text-bramble-800">
          Select All
        </button>
        <span className="text-gray-300">|</span>
        <button onClick={clearAll} className="text-xs text-bramble-600 hover:text-bramble-800">
          Clear
        </button>
      </div>
      <div className="flex flex-wrap gap-2">
        {sensorNodes.map((node) => {
          const selectedIndex = selectedIds.indexOf(node.device_id);
          const isSelected = selectedIndex !== -1;
          const color = isSelected ? colors[selectedIndex % colors.length] : undefined;
          const displayName = node.metadata?.name || node.device_id;

          return (
            <button
              key={node.device_id}
              onClick={() => toggleNode(node.device_id)}
              className={`inline-flex items-center gap-1.5 px-3 py-1.5 rounded-full text-sm transition-colors border ${
                isSelected
                  ? 'border-gray-300 bg-white shadow-sm font-medium'
                  : 'border-gray-200 bg-gray-50 text-gray-600 hover:bg-gray-100'
              }`}
            >
              {isSelected && (
                <span
                  className="w-2.5 h-2.5 rounded-full shrink-0"
                  style={{ backgroundColor: color }}
                />
              )}
              {displayName}
            </button>
          );
        })}
      </div>
    </div>
  );
}

export default SensorPicker;
