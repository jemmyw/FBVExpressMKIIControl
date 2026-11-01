import XCTest
import FBVCore
@testable import PedalKit

@MainActor final class InputTests: XCTestCase {
    private func ready() async throws -> PedalModel {
        let model = PedalModel(session: try CoreSession(demo: true))
        await model.requestRefresh()
        await model.startTesting()
        XCTAssertEqual(model.inputState, .listening)
        return model
    }
    func testMappingCollisionsDraftIndependenceAndUnmatched() async throws {
        let model = try await ready()
        defer { model.stopTesting() }
        let baseline = model.snapshot
        model.ccText = "95"
        XCTAssertTrue(model.session.inputMonitor.demoFeed(port: 1, bytes: [0xb0, 95, 63]))
        model.pollInput()
        XCTAssertEqual(Set(model.inputActivity.keys), [.wah, .volume])
        XCTAssertEqual(model.inputValues[.wah], 63)
        XCTAssertEqual(model.inputValues[.volume], 63)
        XCTAssertEqual(model.selected, .a)
        XCTAssertEqual(model.ccText, "95")
        XCTAssertEqual(model.snapshot, baseline)
        XCTAssertEqual(model.lastInput?.detail, "Port 1 · Ch 1 · CC 95 · 63")
        XCTAssertTrue(model.session.inputMonitor.demoFeed(port: 2, bytes: [0xb0, 20, 127]))
        XCTAssertTrue(model.session.inputMonitor.demoFeed(port: 1, bytes: [0xb1, 20, 127]))
        XCTAssertTrue(model.session.inputMonitor.demoFeed(port: 1, bytes: [0xb0, 74, 127]))
        model.pollInput()
        XCTAssertNil(model.inputValues[.a])
        XCTAssertEqual(model.lastInput?.cc, 74)
        XCTAssertEqual(model.lastInput?.value, 127)
    }
    func testZeroValueFlashesThenSettlesWithoutNewInput() async throws {
        let model = try await ready()
        defer { model.stopTesting() }
        XCTAssertTrue(model.session.inputMonitor.demoFeed(port: 1, bytes: [0xb0, 20, 0]))
        model.pollInput()
        XCTAssertNotNil(model.inputActivity[.a])
        XCTAssertEqual(model.inputValues[.a], 0)
        try await Task.sleep(nanoseconds: 350_000_000)
        model.pollInput()
        XCTAssertTrue(model.inputActivity.isEmpty)
        XCTAssertEqual(model.lastInput?.value, 0)
    }
    func testCCZeroAssignmentUsesLoadedNotDraft() async throws {
        let model = try await ready()
        model.stopTesting()
        model.ccText = "0"; model.review(); await model.applyConfirmed()
        await model.startTesting()
        defer { model.stopTesting() }
        model.ccText = "1"
        XCTAssertTrue(model.session.inputMonitor.demoFeed(port: 1, bytes: [0xb0, 0, 0]))
        model.pollInput()
        XCTAssertEqual(model.inputActivity[.a]?.cc, 0)
        XCTAssertEqual(model.ccText, "1")
    }
    func testStopRestartRefreshDisconnectAndApplyClearActivity() async throws {
        let model = try await ready()
        XCTAssertTrue(model.session.inputMonitor.demoFeed(port: 1, bytes: [0xb0, 95, 127]))
        model.pollInput(); XCTAssertFalse(model.inputValues.isEmpty)
        model.stopTesting()
        XCTAssertEqual(model.inputState, .stopped)
        XCTAssertNil(model.lastInput); XCTAssertTrue(model.inputValues.isEmpty)
        await model.startTesting()
        model.pollInput(); XCTAssertNil(model.lastInput)
        XCTAssertTrue(model.session.inputMonitor.demoDisconnect())
        model.pollInput()
        XCTAssertEqual(model.inputState, .disconnected)
        XCTAssertTrue(model.inputValues.isEmpty)
        XCTAssertTrue(model.inputMessage.contains("restart"))
        await model.startTesting()
        XCTAssertEqual(model.inputState, .listening)
        await model.requestRefresh()
        XCTAssertEqual(model.inputState, .stopped)
        await model.startTesting()
        model.ccText = "42"; model.review()
        await model.applyConfirmed()
        XCTAssertEqual(model.inputState, .stopped)
        XCTAssertNil(model.lastInput)
    }
    func testModelTeardownStopsMonitorEvenWhenSessionIsRetained() async throws {
        let session = try CoreSession(demo: true)
        var model: PedalModel? = PedalModel(session: session)
        await model?.requestRefresh()
        await model?.startTesting()
        XCTAssertEqual(session.inputMonitor.drain().0, .listening)
        model = nil
        XCTAssertEqual(session.inputMonitor.drain().0, .stopped)
    }
    func testStartGuardsAndRealInjectionRefusal() async throws {
        let model = PedalModel(session: try CoreSession(demo: true))
        await model.startTesting()
        XCTAssertEqual(model.inputState, .stopped)
        await model.requestRefresh()
        model.ccText = "42"; model.review()
        await model.startTesting()
        XCTAssertEqual(model.inputState, .stopped)
        model.cancelReview(); model.select(.b)
        XCTAssertNotNil(model.discardIntent)
        await model.startTesting()
        XCTAssertEqual(model.inputState, .stopped)
        let real = try InputMonitor(demo: false)
        XCTAssertFalse(real.demoFeed(port: 1, bytes: [0xb0, 20, 127]))
        XCTAssertFalse(real.demoDisconnect())
        real.stop()
    }
}
