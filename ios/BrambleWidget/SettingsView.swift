import SwiftUI
import WidgetKit

/// The host app's only screen. Configures the API connection (base URL + secrets) and
/// the list of valves the widget shows. Secrets are written to the Keychain; the base
/// URL and valve list to the shared App Group defaults so the widget can read them.
struct SettingsView: View {
    @State private var baseUrl: String = AppSettings.baseUrl
    @State private var apiToken: String = KeychainStore.get(.apiToken) ?? ""
    @State private var cfClientId: String = KeychainStore.get(.cfClientId) ?? ""
    @State private var cfClientSecret: String = KeychainStore.get(.cfClientSecret) ?? ""
    @State private var valves: [ValveConfig] = AppSettings.valves
    @State private var connectionResult: String?
    @State private var isTesting: Bool = false
    @State private var isSyncingNames: Bool = false

    var body: some View {
        NavigationStack {
            Form {
                Section("Connection") {
                    TextField("Base URL", text: $baseUrl)
                        .textInputAutocapitalization(.never)
                        .autocorrectionDisabled()
                        .keyboardType(.URL)
                    SecureField("API Token (Bearer)", text: $apiToken)
                    SecureField("CF-Access-Client-Id", text: $cfClientId)
                    SecureField("CF-Access-Client-Secret", text: $cfClientSecret)
                    Button(action: testConnection) {
                        HStack {
                            Text("Test Connection")
                            if isTesting { ProgressView().padding(.leading, 4) }
                        }
                    }
                    .disabled(isTesting)
                    if let connectionResult {
                        Text(connectionResult).font(.footnote).foregroundStyle(.secondary)
                    }
                }

                Section("Valves") {
                    ForEach($valves) { $valve in
                        ValveEditor(valve: $valve)
                    }
                    .onDelete { valves.remove(atOffsets: $0) }
                    Button {
                        valves.append(ValveConfig(label: "New Valve", deviceId: "", valve: 0))
                    } label: {
                        Label("Add Valve", systemImage: "plus")
                    }
                    Button(action: syncNamesFromServer) {
                        HStack {
                            Label("Sync names from server", systemImage: "arrow.down.circle")
                            if isSyncingNames { Spacer(); ProgressView() }
                        }
                    }
                    .disabled(isSyncingNames)
                }
            }
            .navigationTitle("Bramble Valves")
            .toolbar {
                ToolbarItem(placement: .confirmationAction) {
                    Button("Save", action: save)
                }
            }
        }
    }

    private func save() {
        AppSettings.baseUrl = baseUrl.trimmingCharacters(in: .whitespaces)
        AppSettings.valves = valves
        KeychainStore.set(apiToken, for: .apiToken)
        KeychainStore.set(cfClientId, for: .cfClientId)
        KeychainStore.set(cfClientSecret, for: .cfClientSecret)
        WidgetCenter.shared.reloadAllTimelines()
        pushValveNames()
    }

    /// Server is the source of truth for valve names, so renaming a valve here
    /// writes the label back to the API (best-effort). Dashboard and widget then
    /// agree. Only configs with a device ID are pushed.
    private func pushValveNames() {
        let client = BrambleAPIClient()
        let snapshot = valves
        Task {
            for valve in snapshot where !valve.deviceId.isEmpty {
                try? await client.updateValveName(
                    deviceId: valve.deviceId, valve: valve.valve, name: valve.label)
            }
        }
    }

    /// Pull canonical names from the server into the local configs so a rename
    /// made elsewhere (e.g. the dashboard) shows up here and in the widget.
    private func syncNamesFromServer() {
        isSyncingNames = true
        let client = BrambleAPIClient()
        let snapshot = valves
        Task {
            var updated = snapshot
            for index in updated.indices where !updated[index].deviceId.isEmpty {
                if let name = await client.fetchValveName(
                    deviceId: updated[index].deviceId, valve: updated[index].valve),
                    !name.isEmpty {
                    updated[index].label = name
                }
            }
            await MainActor.run {
                valves = updated
                AppSettings.valves = updated
                WidgetCenter.shared.reloadAllTimelines()
                isSyncingNames = false
            }
        }
    }

    private func testConnection() {
        save()
        isTesting = true
        connectionResult = nil
        Task {
            do {
                try await BrambleAPIClient().checkHealth()
                connectionResult = "Connected ✓"
            } catch {
                connectionResult = error.localizedDescription
            }
            isTesting = false
        }
    }
}

struct ValveEditor: View {
    @Binding var valve: ValveConfig

    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            TextField("Label", text: $valve.label)
            TextField("Device ID", text: $valve.deviceId)
                .keyboardType(.numberPad)
            Stepper("Valve index: \(valve.valve)", value: $valve.valve, in: 0...99)
            Stepper(
                "Duration: \(valve.defaultDurationSeconds / 60) min",
                value: $valve.defaultDurationSeconds, in: 60...7200, step: 60)
            Toggle(isOn: $valve.isPrimary) {
                Label("Show on Lock Screen", systemImage: "lock")
            }
        }
        .padding(.vertical, 4)
    }
}
