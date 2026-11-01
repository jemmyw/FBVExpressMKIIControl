import AppKit
import SwiftUI
import PedalKit

@MainActor enum NativeSmoke {
    private static var renderedViews: [NSHostingView<PedalView>] = []
    static func run(directory: String) -> Never {
        let app = NSApplication.shared
        app.setActivationPolicy(.prohibited)
        Task { @MainActor in
            do {
                let destination = URL(fileURLWithPath: directory, isDirectory: true)
                try FileManager.default.createDirectory(at: destination, withIntermediateDirectories: true)
                let session = try CoreSession(demo: true)
                let model = PedalModel(session: session)
                await model.requestRefresh()
                guard model.loaded?.cc == 20 else { throw SmokeError.failed("Initial A assignment") }
                try render(model, to: destination.appendingPathComponent("01-switch-a.png"))
                await model.startTesting()
                guard model.inputState == .listening,
                      session.inputMonitor.demoFeed(port: 1, bytes: [0xb0, 20, 0, 95, 96]) else {
                    throw SmokeError.failed("Monitor start and sample")
                }
                model.pollInput()
                guard model.inputActivity[.a]?.value == 0,
                      model.inputValues[.wah] == 96, model.inputValues[.volume] == 96,
                      model.selected == .a, !model.dirty else { throw SmokeError.failed("Monitor mapping") }
                try render(model, to: destination.appendingPathComponent("04-live-input.png"))
                model.stopTesting()
                guard model.lastInput == nil, model.inputValues.isEmpty else { throw SmokeError.failed("Monitor stop") }
                model.select(.d)
                model.ccText = "42"
                guard model.preview?.status == .preview else { throw SmokeError.failed("D preview") }
                model.review()
                guard model.confirmation?.control == .d else { throw SmokeError.failed("D confirmation") }
                model.cancelReview()
                try render(model, to: destination.appendingPathComponent("02-switch-d-draft.png"))
                model.select(.volume)
                guard model.discardIntent == .select(.volume) else { throw SmokeError.failed("Discard guard") }
                await model.resolveDiscard(true)
                guard model.selected == .volume else { throw SmokeError.failed("Volume selection") }
                model.ccText = "74"
                model.review()
                guard model.confirmation?.before.cc == 95 else { throw SmokeError.failed("Volume confirmation") }
                await model.applyConfirmed()
                guard model.loaded?.cc == 74, model.lastStatus == .applied else { throw SmokeError.failed("Verified demo apply") }
                try render(model, to: destination.appendingPathComponent("03-volume-applied.png"))
                let report = "{\"mode\":\"demo-only\",\"midi_opened\":false,\"checks\":[\"initial read\",\"D select\",\"draft\",\"preview\",\"confirmation\",\"cancel\",\"discard guard\",\"volume select\",\"apply\",\"verified readback\",\"monitor start/sample/collision/stop\",\"four native renders\"]}\n"
                try report.write(to: destination.appendingPathComponent("actions.json"), atomically: true, encoding: .utf8)
                print("Native smoke passed. Four SwiftUI snapshots: \(directory)")
                exit(0)
            } catch {
                fputs("Native smoke failed: \(error)\n", stderr)
                exit(1)
            }
        }
        app.run()
        exit(1)
    }
    private static func render(_ model: PedalModel, to path: URL) throws {
        let view = NSHostingView(rootView: PedalView(model: model))
        renderedViews.append(view)
        view.frame = NSRect(x: 0, y: 0, width: 1040, height: 600)
        view.layoutSubtreeIfNeeded()
        guard view.fittingSize.height <= 600, view.fittingSize.width <= 1040 else {
            throw SmokeError.failed("View exceeds 1040×600: \(view.fittingSize)")
        }
        guard let bitmap = view.bitmapImageRepForCachingDisplay(in: view.bounds) else {
            throw SmokeError.failed("Could not create native bitmap")
        }
        view.cacheDisplay(in: view.bounds, to: bitmap)
        guard let data = bitmap.representation(using: .png, properties: [:]) else {
            throw SmokeError.failed("Could not encode native bitmap")
        }
        try data.write(to: path)
    }
    private enum SmokeError: Error { case failed(String) }
}
