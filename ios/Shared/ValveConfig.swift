import Foundation

/// A single valve the widget can control.
///
/// `deviceId` is the node's 64-bit hardware unique ID (the same value the REST API
/// expects in `/api/nodes/{device_id}/...`). `valve` is the 0-based valve index on
/// that node. `defaultDurationSeconds` is how long a run-once tap waters for.
struct ValveConfig: Codable, Identifiable, Hashable {
    var id: UUID
    var label: String
    var deviceId: String
    var valve: Int
    var defaultDurationSeconds: Int

    init(
        id: UUID = UUID(),
        label: String,
        deviceId: String,
        valve: Int,
        defaultDurationSeconds: Int = 900
    ) {
        self.id = id
        self.label = label
        self.deviceId = deviceId
        self.valve = valve
        self.defaultDurationSeconds = defaultDurationSeconds
    }
}
