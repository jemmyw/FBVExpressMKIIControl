import XCTest
import FBVCore
@testable import PedalKit

final class CoreTests: XCTestCase {
    func testFixtureAndPreviewUsePublicCore() throws {
        let session = try CoreSession(demo: true)
        let snapshot = session.read().dump
        for control in Control.allCases {
            let current = try XCTUnwrap(session.assignment(snapshot: snapshot, control: control))
            XCTAssertEqual(current.port, 1)
            XCTAssertEqual(current.channel, 1)
            XCTAssertEqual(current.cc, control.isExpression ? 95 : 20 + control.rawValue)
            XCTAssertEqual(session.preview(snapshot: snapshot, control: control, draft: current).status, .noOp)
            var draft = current; draft.cc = 74
            let preview = session.preview(snapshot: snapshot, control: control, draft: draft)
            XCTAssertEqual(preview.status, .preview)
            XCTAssertEqual(session.read().dump, snapshot, "Preview must never mutate even a mock device")
            for index in snapshot.indices where index != 9 + control.rawValue * 10 + 6 &&
                index != 9 + control.rawValue * 10 + 8 && index != 79 {
                XCTAssertEqual(preview.expected[index], snapshot[index])
            }
        }
    }

    func testNoOpHasNoBackupOrAssignment() throws {
        let session = try CoreSession(demo: true)
        let snapshot = session.read().dump
        let draft = try XCTUnwrap(session.assignment(snapshot: snapshot, control: .a))
        session.setDemoScenario(7)
        let result = session.apply(snapshot: snapshot, control: .a, draft: draft)
        XCTAssertEqual(result.status, .noOp, "No-op must bypass the failing backup callback")
        XCTAssertEqual(result.backup, "")
        XCTAssertEqual(session.read().dump, snapshot)
    }

    func testFaultsAtTransactionBarriers() throws {
        let expected: [(Int, Status)] = [(1, .disconnected), (3, .drift), (4, .drift),
                                         (5, .uncertain), (6, .uncertain), (7, .backupFailed)]
        for (scenario, status) in expected {
            let session = try CoreSession(demo: true)
            let snapshot = session.read().dump
            var draft = try XCTUnwrap(session.assignment(snapshot: snapshot, control: .a))
            draft.cc = 42
            session.setDemoScenario(scenario)
            let result = session.apply(snapshot: snapshot, control: .a, draft: draft)
            XCTAssertEqual(result.status, status, "Scenario \(scenario)")
            XCTAssertFalse(result.message.isEmpty)
        }
    }

    func testVerifiedApplyAndStaleSnapshot() throws {
        let session = try CoreSession(demo: true)
        let snapshot = session.read().dump
        var draft = try XCTUnwrap(session.assignment(snapshot: snapshot, control: .volume))
        draft.cc = 74
        let result = session.apply(snapshot: snapshot, control: .volume, draft: draft)
        XCTAssertEqual(result.status, .applied)
        XCTAssertEqual(session.read().dump, result.expected)
        XCTAssertEqual(session.apply(snapshot: snapshot, control: .volume, draft: draft).status, .drift)
    }

    func testUnsupportedAndInvalidBoundaries() throws {
        let session = try CoreSession(demo: true)
        session.setDemoScenario(2)
        let snapshot = session.read().dump
        XCTAssertNil(session.assignment(snapshot: snapshot, control: .a))
        XCTAssertNotNil(session.assignment(snapshot: snapshot, control: .b), "Unsupported unrelated records are preserved")
        let draft = Assignment(cc: 42, port: 1, channel: 1, mode: 3)
        XCTAssertEqual(session.preview(snapshot: snapshot, control: .a, draft: draft).status, .unsupported)
        let edit = FbvEdit(pedal: 0, cc: 42, port: 1, channel: 1, mode: 3)
        XCTAssertEqual(fbv_preview(nil, 81, edit).status, FBV_INVALID)
        XCTAssertEqual(fbv_preview(nil, 0, edit).status, FBV_INVALID)
        XCTAssertEqual(fbv_session_apply(nil, nil, 0, edit).status, FBV_INVALID)
        XCTAssertEqual(fbv_session_read(nil).status, FBV_INVALID)
        XCTAssertFalse(fbv_assignment(nil, 81, 0, nil))
        for size in [0, 1, 80, 82, 100] {
            let bytes = Array(repeating: UInt8(0), count: size)
            XCTAssertEqual(session.preview(snapshot: bytes, control: .a, draft: draft).status, .invalid)
        }
        for invalid in [Assignment(cc: -2, port: 1, channel: 1, mode: 3),
                        Assignment(cc: 128, port: 1, channel: 1, mode: 3),
                        Assignment(cc: 42, port: 0, channel: 1, mode: 3),
                        Assignment(cc: 42, port: 4, channel: 1, mode: 3),
                        Assignment(cc: 42, port: 1, channel: 17, mode: 3),
                        Assignment(cc: 42, port: 1, channel: 1, mode: 4)] {
            XCTAssertEqual(session.preview(snapshot: snapshot, control: .a, draft: invalid).status, .invalid)
        }
        XCTAssertEqual(session.preview(snapshot: snapshot, control: .wah, draft: draft).status, .invalid)
    }
}
