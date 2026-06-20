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

    /// Whether this valve appears on single-action surfaces (Lock Screen accessory
    /// widgets and, later, Control Center / Action Button). Those surfaces have room
    /// for only one or two valves, so they show the primaries.
    var isPrimary: Bool

    init(
        id: UUID = UUID(),
        label: String,
        deviceId: String,
        valve: Int,
        defaultDurationSeconds: Int = 900,
        isPrimary: Bool = false
    ) {
        self.id = id
        self.label = label
        self.deviceId = deviceId
        self.valve = valve
        self.defaultDurationSeconds = defaultDurationSeconds
        self.isPrimary = isPrimary
    }

    // Custom decoding so adding `isPrimary` doesn't break configs persisted before it.
    enum CodingKeys: String, CodingKey {
        case id, label, deviceId, valve, defaultDurationSeconds, isPrimary
    }

    init(from decoder: Decoder) throws {
        let container = try decoder.container(keyedBy: CodingKeys.self)
        id = try container.decode(UUID.self, forKey: .id)
        label = try container.decode(String.self, forKey: .label)
        deviceId = try container.decode(String.self, forKey: .deviceId)
        valve = try container.decode(Int.self, forKey: .valve)
        defaultDurationSeconds = try container.decode(Int.self, forKey: .defaultDurationSeconds)
        isPrimary = try container.decodeIfPresent(Bool.self, forKey: .isPrimary) ?? false
    }
}
