import SwiftUI

@main
struct HengeApp: App {
    @StateObject private var runner = M1Runner()

    var body: some Scene {
        WindowGroup {
            ContentView(runner: runner)
        }
    }
}
