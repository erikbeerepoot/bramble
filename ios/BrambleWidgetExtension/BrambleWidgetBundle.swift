import WidgetKit
import SwiftUI

@main
struct BrambleWidgetBundle: WidgetBundle {
    var body: some Widget {
        ValveWidget()
        ValveLockScreenWidget()
    }
}
