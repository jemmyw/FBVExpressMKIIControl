import XCTest
@testable import PedalKit

@MainActor final class ModelTests: XCTestCase {
    private func ready() async throws -> PedalModel {
        let model = PedalModel(session: try CoreSession(demo: true))
        XCTAssertNil(model.snapshot)
        XCTAssertFalse(model.canReview)
        await model.requestRefresh()
        XCTAssertEqual(model.lastStatus, .read)
        return model
    }

    func testSelectionDraftResetAndDiscard() async throws {
        let model = try await ready()
        for control in Control.allCases {
            model.select(control)
            XCTAssertEqual(model.selected, control)
            XCTAssertFalse(model.dirty)
            XCTAssertNotNil(model.loaded)
        }
        model.select(.c)
        model.ccText = "42"
        model.select(.d)
        XCTAssertEqual(model.selected, .c)
        XCTAssertEqual(model.discardIntent, .select(.d))
        await model.resolveDiscard(false)
        XCTAssertEqual(model.ccText, "42")
        model.select(.d)
        await model.resolveDiscard(true)
        XCTAssertEqual(model.selected, .d)
        XCTAssertEqual(model.ccText, "23")
        model.ccText = "51"
        model.discardDraft()
        XCTAssertEqual(model.ccText, "23")
        XCTAssertFalse(model.dirty)
    }

    func testValidationAndNoOpCannotApply() async throws {
        let model = try await ready()
        XCTAssertEqual(model.preview?.status, .noOp)
        model.review()
        XCTAssertNil(model.confirmation)
        await model.applyConfirmed()
        XCTAssertEqual(model.lastStatus, .read)
        for invalid in ["", "-1", "128", "1.5", "+1", "cat", String(repeating: "9", count: 100)] {
            model.ccText = invalid
            XCTAssertNil(model.draft)
            XCTAssertNotNil(model.validation)
            XCTAssertFalse(model.canReview)
        }
        model.ccText = "0"
        XCTAssertNil(model.validation)
        XCTAssertTrue(model.canReview)
    }

    func testPreviewConfirmationCancelAndApply() async throws {
        let model = try await ready()
        model.select(.wah)
        let snapshot = try XCTUnwrap(model.snapshot)
        model.ccText = "74"; model.port = 2; model.channel = 16
        XCTAssertEqual(model.draft?.mode, -1)
        XCTAssertEqual(model.preview?.status, .preview)
        XCTAssertEqual(model.session.read().dump, snapshot)
        model.review()
        XCTAssertEqual(model.confirmation?.control, .wah)
        XCTAssertEqual(model.confirmation?.before.cc, 95)
        XCTAssertEqual(model.confirmation?.after.cc, 74)
        XCTAssertTrue(model.locked)
        model.select(.b)
        XCTAssertEqual(model.selected, .wah)
        model.cancelReview()
        XCTAssertNil(model.confirmation)
        XCTAssertTrue(model.dirty)
        model.review()
        await model.applyConfirmed()
        XCTAssertEqual(model.lastStatus, .applied)
        XCTAssertEqual(model.loaded?.cc, 74)
        XCTAssertEqual(model.loaded?.port, 2)
        XCTAssertEqual(model.loaded?.channel, 16)
        XCTAssertFalse(model.dirty)
        XCTAssertEqual(model.backup, "", "Demo creates no backup files")
    }

    func testRefreshRequiresDeliberateDiscard() async throws {
        let model = try await ready()
        model.ccText = "42"
        await model.requestRefresh()
        XCTAssertEqual(model.discardIntent, .refresh)
        XCTAssertEqual(model.ccText, "42")
        await model.resolveDiscard(false)
        XCTAssertEqual(model.ccText, "42")
        await model.requestRefresh()
        await model.resolveDiscard(true)
        XCTAssertEqual(model.ccText, "20")
        XCTAssertFalse(model.dirty)
    }

    func testApplyFailuresInvalidateSnapshotAndRequireRefresh() async throws {
        for (scenario, expected) in [(1, Status.disconnected), (3, .drift), (4, .drift),
                                      (5, .uncertain), (6, .uncertain), (7, .backupFailed)] {
            let model = try await ready()
            model.ccText = "42"; model.review()
            model.session.setDemoScenario(scenario)
            await model.applyConfirmed()
            XCTAssertEqual(model.lastStatus, expected)
            XCTAssertNil(model.snapshot)
            XCTAssertNil(model.loaded)
            XCTAssertNil(model.confirmation)
            XCTAssertFalse(model.canReview)
            XCTAssertFalse(model.busy)
            XCTAssertFalse(model.message.isEmpty)
            await model.applyConfirmed()
            XCTAssertEqual(model.lastStatus, expected, "No implicit retry")
            model.session.setDemoScenario(0)
            await model.requestRefresh()
            XCTAssertNotNil(model.loaded)
        }
    }

    func testUnsupportedAndDisconnectedAreTruthful() async throws {
        let model = try await ready()
        model.session.setDemoScenario(2)
        await model.requestRefresh()
        XCTAssertNotNil(model.snapshot)
        XCTAssertNil(model.loaded)
        XCTAssertFalse(model.canReview)
        model.select(.b)
        XCTAssertNotNil(model.loaded)
        model.session.setDemoScenario(1)
        await model.requestRefresh()
        XCTAssertNil(model.snapshot)
        XCTAssertEqual(model.lastStatus, .disconnected)
        XCTAssertFalse(model.canReview)
    }

    func testBusySerialisesDoubleApplyAndSelection() async throws {
        let model = try await ready()
        model.ccText = "42"; model.review()
        async let first: Void = model.applyConfirmed()
        async let second: Void = model.applyConfirmed()
        await first; await second
        XCTAssertEqual(model.lastStatus, .applied)
        XCTAssertEqual(model.loaded?.cc, 42)
        XCTAssertFalse(model.busy)
    }
}
