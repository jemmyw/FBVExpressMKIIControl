import Foundation
import FBVCore

public enum Control: Int, CaseIterable, Identifiable, Sendable {
    case a, b, c, d, wah, volume
    public var id: Int { rawValue }
    public var title: String { ["A", "B", "C", "D", "Wah", "Volume"][rawValue] }
    public var isExpression: Bool { rawValue >= 4 }
}

public struct Assignment: Equatable, Sendable {
    public var cc: Int
    public var port: Int
    public var channel: Int
    public var mode: Int
    public var summary: String {
        let modeName = mode == -1 ? "Continuous" : ["", "Single", "Toggle", "Momentary"][max(0, min(3, mode))]
        return "CC \(cc) · Port \(port) · Channel \(channel) · \(modeName)"
    }
    public init(cc: Int, port: Int, channel: Int, mode: Int) {
        self.cc = cc; self.port = port; self.channel = channel; self.mode = mode
    }
    func edit(for control: Control) -> FbvEdit {
        FbvEdit(pedal: Int32(control.rawValue), cc: Int32(clamping: cc),
                port: Int32(clamping: port), channel: Int32(clamping: channel),
                mode: Int32(clamping: mode))
    }
}

public enum Status: Equatable, Sendable {
    case read, preview, noOp, applied, invalid, unsupported, disconnected, drift, backupFailed, uncertain
    init(_ value: FbvStatus) {
        switch value {
        case FBV_READ: self = .read
        case FBV_PREVIEW: self = .preview
        case FBV_NOOP: self = .noOp
        case FBV_APPLIED: self = .applied
        case FBV_INVALID: self = .invalid
        case FBV_UNSUPPORTED: self = .unsupported
        case FBV_DISCONNECTED: self = .disconnected
        case FBV_DRIFT: self = .drift
        case FBV_BACKUP_FAILED: self = .backupFailed
        default: self = .uncertain
        }
    }
}

public struct CoreResult: Sendable {
    public let status: Status
    public let dump: [UInt8]
    public let expected: [UInt8]
    public let assignment: [UInt8]
    public let backup: String
    public let message: String
    init(_ value: FbvResult) {
        status = Status(value.status)
        dump = withUnsafeBytes(of: value.dump) { Array($0) }
        expected = withUnsafeBytes(of: value.expected) { Array($0) }
        assignment = withUnsafeBytes(of: value.assignment) { Array($0) }
        backup = withUnsafeBytes(of: value.backup) { String(decoding: $0.prefix(while: { $0 != 0 }), as: UTF8.self) }
        message = withUnsafeBytes(of: value.message) { String(decoding: $0.prefix(while: { $0 != 0 }), as: UTF8.self) }
    }
    public var sysex: String { assignment.map { String(format: "%02X", $0) }.joined(separator: " ") }
}

public final class CoreSession: @unchecked Sendable {
    private let handle: OpaquePointer
    private let lock = NSLock()
    public let demo: Bool
    public let inputMonitor: InputMonitor
    public let backupDirectory: URL

    public init(demo: Bool, backupDirectory: URL? = nil) throws {
        self.demo = demo
        self.inputMonitor = try InputMonitor(demo: demo)
        self.backupDirectory = backupDirectory ?? FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("FBV/Backups", isDirectory: true)
        if !demo {
            try FileManager.default.createDirectory(at: self.backupDirectory, withIntermediateDirectories: true,
                                                    attributes: [.posixPermissions: 0o700])
        }
        guard let handle = fbv_session_create(demo, self.backupDirectory.path) else {
            throw NSError(domain: "FBV", code: 1, userInfo: [NSLocalizedDescriptionKey: "Could not create a session. Backup path may be too long."])
        }
        self.handle = handle
    }
    deinit { fbv_session_destroy(handle) }
    public func read() -> CoreResult {
        lock.lock(); defer { lock.unlock() }
        return CoreResult(fbv_session_read(handle))
    }
    public func apply(snapshot: [UInt8], control: Control, draft: Assignment) -> CoreResult {
        lock.lock(); defer { lock.unlock() }
        return snapshot.withUnsafeBufferPointer {
            CoreResult(fbv_session_apply(handle, $0.baseAddress, $0.count, draft.edit(for: control)))
        }
    }
    public func preview(snapshot: [UInt8], control: Control, draft: Assignment) -> CoreResult {
        snapshot.withUnsafeBufferPointer {
            CoreResult(fbv_preview($0.baseAddress, $0.count, draft.edit(for: control)))
        }
    }
    public func assignment(snapshot: [UInt8], control: Control) -> Assignment? {
        var edit = FbvEdit()
        let supported = snapshot.withUnsafeBufferPointer {
            fbv_assignment($0.baseAddress, $0.count, Int32(control.rawValue), &edit)
        }
        guard supported else { return nil }
        return Assignment(cc: Int(edit.cc), port: Int(edit.port), channel: Int(edit.channel), mode: Int(edit.mode))
    }
    @discardableResult public func setDemoScenario(_ value: Int) -> Bool {
        lock.lock(); defer { lock.unlock() }
        return fbv_demo_scenario(handle, Int32(clamping: value))
    }
}
