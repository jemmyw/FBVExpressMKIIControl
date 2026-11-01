import AppKit
import SwiftUI
import PedalKit

@main struct FBVApp: App {
    @StateObject private var model: PedalModel

    init() {
        if let index = CommandLine.arguments.firstIndex(of: "--snapshot"),
           CommandLine.arguments.indices.contains(index + 1) {
            NativeSmoke.run(directory: CommandLine.arguments[index + 1])
        }
        do {
            let session = try CoreSession(demo: CommandLine.arguments.contains("--demo"))
            _model = StateObject(wrappedValue: PedalModel(session: session))
        } catch {
            let alert = NSAlert()
            alert.messageText = "Could not start FBV"
            alert.informativeText = error.localizedDescription
            alert.runModal()
            exit(1)
        }
    }
    var body: some Scene {
        WindowGroup("FBV · Assignment studio") {
            PedalView(model: model)
                .task { if model.snapshot == nil { await model.requestRefresh() } }
        }
        .windowStyle(.hiddenTitleBar)
        .defaultSize(width: 1040, height: 600)
        .windowResizability(.contentMinSize)
        .commands {
            CommandGroup(replacing: .newItem) {}
        }
    }
}
