import WidgetKit
import SwiftUI
import AppIntents

/// A snapshot of the configured valves and their current UI state.
struct ValveEntry: TimelineEntry {
    let date: Date
    let valves: [ValveConfig]
    let states: [UUID: ValveState]
}

struct ValveTimelineProvider: TimelineProvider {
    func placeholder(in context: Context) -> ValveEntry {
        ValveEntry(date: Date(), valves: ValveWidgetPreview.sampleValves, states: [:])
    }

    func getSnapshot(in context: Context, completion: @escaping (ValveEntry) -> Void) {
        completion(currentEntry())
    }

    func getTimeline(in context: Context, completion: @escaping (Timeline<ValveEntry>) -> Void) {
        // State changes are driven by button taps (which reload the timeline), so a
        // single never-expiring entry is sufficient.
        completion(Timeline(entries: [currentEntry()], policy: .never))
    }

    private func currentEntry() -> ValveEntry {
        let valves = AppSettings.valves
        var states: [UUID: ValveState] = [:]
        for valve in valves {
            states[valve.id] = ValveStateStore.state(deviceId: valve.deviceId, valve: valve.valve)
        }
        return ValveEntry(date: Date(), valves: valves, states: states)
    }
}

struct ValveWidgetEntryView: View {
    var entry: ValveEntry

    var body: some View {
        if entry.valves.isEmpty {
            Text("Add valves in the Bramble app")
                .font(.caption)
                .foregroundStyle(.secondary)
                .multilineTextAlignment(.center)
                .padding()
        } else {
            VStack(alignment: .leading, spacing: 8) {
                ForEach(entry.valves) { valve in
                    ValveRow(valve: valve, state: entry.states[valve.id]?.state ?? .idle)
                }
            }
            .padding(12)
        }
    }
}

/// One valve: label + a Run and a Stop button. Each button fires an App Intent that
/// performs the API call directly from the widget process (iOS 17+).
struct ValveRow: View {
    let valve: ValveConfig
    let state: ValveRunState

    var body: some View {
        HStack(spacing: 8) {
            Circle()
                .fill(statusColor)
                .frame(width: 8, height: 8)
            Text(valve.label)
                .font(.subheadline.weight(.medium))
                .lineLimit(1)
            Spacer()
            Button(intent: RunValveIntent(
                deviceId: valve.deviceId,
                valve: valve.valve,
                durationSeconds: valve.defaultDurationSeconds
            )) {
                Image(systemName: "drop.fill")
            }
            .tint(.blue)
            Button(intent: StopValveIntent(deviceId: valve.deviceId, valve: valve.valve)) {
                Image(systemName: "stop.fill")
            }
            .tint(.red)
        }
    }

    private var statusColor: Color {
        switch state {
        case .idle: return .gray
        case .pending: return .yellow
        case .running: return .green
        case .error: return .red
        }
    }
}

struct ValveWidget: Widget {
    let kind: String = "BrambleValveWidget"

    var body: some WidgetConfiguration {
        StaticConfiguration(kind: kind, provider: ValveTimelineProvider()) { entry in
            ValveWidgetEntryView(entry: entry)
                .containerBackground(.fill.tertiary, for: .widget)
        }
        .configurationDisplayName("Valves")
        .description("Start and stop irrigation valves.")
        .supportedFamilies([.systemSmall, .systemMedium, .systemLarge])
    }
}

enum ValveWidgetPreview {
    static let sampleValves: [ValveConfig] = [
        ValveConfig(label: "Greenhouse 1", deviceId: "1311768467463790320", valve: 0),
        ValveConfig(label: "Greenhouse 2", deviceId: "1311768467463790320", valve: 1),
    ]
}
