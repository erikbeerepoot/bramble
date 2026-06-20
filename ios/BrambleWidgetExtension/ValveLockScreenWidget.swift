import WidgetKit
import SwiftUI
import AppIntents

/// Lock Screen (accessory) widget for the **primary** valves. Real estate is tiny, so
/// these surfaces use a single toggle per valve (`ToggleValveIntent`): tap runs an idle
/// valve, tap again stops it — the shortest path from intent to task on the Lock Screen.
struct ValveLockScreenWidget: Widget {
    let kind: String = "BrambleValveLockScreenWidget"

    var body: some WidgetConfiguration {
        StaticConfiguration(kind: kind, provider: ValveTimelineProvider()) { entry in
            ValveAccessoryView(entry: entry)
                .containerBackground(.clear, for: .widget)
        }
        .configurationDisplayName("Valves (Lock Screen)")
        .description("Quick-tap your primary valves.")
        .supportedFamilies([.accessoryCircular, .accessoryRectangular, .accessoryInline])
    }
}

struct ValveAccessoryView: View {
    @Environment(\.widgetFamily) private var family
    var entry: ValveEntry

    private var primaries: [ValveConfig] {
        entry.valves.filter(\.isPrimary)
    }

    var body: some View {
        switch family {
        case .accessoryCircular:
            circular
        case .accessoryRectangular:
            rectangular
        case .accessoryInline:
            inline
        default:
            EmptyView()
        }
    }

    // A single primary valve as a tappable circular toggle.
    @ViewBuilder private var circular: some View {
        if let valve = primaries.first {
            let state = entry.states[valve.id]?.state ?? .idle
            ToggleButton(valve: valve, state: state) {
                ZStack {
                    AccessoryWidgetBackground()
                    Image(systemName: state.symbolName)
                        .font(.title2)
                }
            }
            .widgetAccentable()
        } else {
            placeholderCircular
        }
    }

    // Up to two primary valves, each a labeled toggle row.
    @ViewBuilder private var rectangular: some View {
        if primaries.isEmpty {
            Text("No primary valves")
                .font(.caption2)
                .foregroundStyle(.secondary)
        } else {
            VStack(alignment: .leading, spacing: 4) {
                ForEach(primaries.prefix(2)) { valve in
                    let state = entry.states[valve.id]?.state ?? .idle
                    ToggleButton(valve: valve, state: state) {
                        HStack(spacing: 6) {
                            Image(systemName: state.symbolName)
                            Text(valve.label).lineLimit(1)
                            Spacer()
                        }
                    }
                }
            }
        }
    }

    // Inline accessories are text-only (no interactive buttons): show a summary.
    @ViewBuilder private var inline: some View {
        let running = primaries.filter { (entry.states[$0.id]?.state ?? .idle) == .running }.count
        Label("\(running) valve\(running == 1 ? "" : "s") running", systemImage: "drop.fill")
    }

    @ViewBuilder private var placeholderCircular: some View {
        ZStack {
            AccessoryWidgetBackground()
            Image(systemName: "drop")
                .foregroundStyle(.secondary)
        }
    }
}

/// A button wired to `ToggleValveIntent` with arbitrary label content.
private struct ToggleButton<Label: View>: View {
    let valve: ValveConfig
    let state: ValveRunState
    @ViewBuilder let label: () -> Label

    var body: some View {
        Button(intent: ToggleValveIntent(
            deviceId: valve.deviceId,
            valve: valve.valve,
            durationSeconds: valve.defaultDurationSeconds
        )) {
            label()
        }
        .buttonStyle(.plain)
    }
}

private extension ValveRunState {
    /// SF Symbol shown for each state on accessory surfaces.
    var symbolName: String {
        switch self {
        case .idle: return "drop"
        case .pending: return "hourglass"
        case .running: return "drop.fill"
        case .error: return "exclamationmark.triangle"
        }
    }
}
