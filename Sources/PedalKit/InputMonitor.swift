import Foundation
import FBVCore

public struct InputEvent: Equatable, Sendable {
    public let port: Int
    public let channel: Int
    public let cc: Int
    public let value: Int
    public let received: Double
    init(_ event: FbvCCEvent) {
        port = Int(event.port); channel = Int(event.channel)
        cc = Int(event.cc); value = Int(event.value); received = event.received
    }
    public var detail: String { "Port \(port) · Ch \(channel) · CC \(cc) · \(value)" }
}

public enum InputState: Sendable { case stopped, listening, disconnected }

public final class InputMonitor: @unchecked Sendable {
    private let handle: OpaquePointer
    private let lock = NSLock()
    public init(demo: Bool) throws {
        guard let handle = fbv_monitor_create(demo) else {
            throw NSError(domain: "FBV", code: 2, userInfo: [NSLocalizedDescriptionKey: "Could not create input monitor."])
        }
        self.handle = handle
    }
    deinit { fbv_monitor_destroy(handle) }
    @discardableResult public func start() -> Bool {
        lock.lock(); defer { lock.unlock() }
        return fbv_monitor_start(handle)
    }
    public func stop() {
        lock.lock(); defer { lock.unlock() }
        fbv_monitor_stop(handle)
    }
    public func drain() -> (InputState, [InputEvent], UInt64) {
        lock.lock(); defer { lock.unlock() }
        var events = Array(repeating: FbvCCEvent(), count: 256)
        var count = 0
        var dropped: UInt64 = 0
        let state = fbv_monitor_drain(handle, &events, events.count, &count, &dropped)
        return (state == FBV_MONITOR_LISTENING ? .listening : state == FBV_MONITOR_DISCONNECTED ? .disconnected : .stopped,
                events.prefix(count).map(InputEvent.init), dropped)
    }
    @discardableResult public func demoFeed(port: Int, bytes: [UInt8]) -> Bool {
        lock.lock(); defer { lock.unlock() }
        return bytes.withUnsafeBufferPointer {
            fbv_monitor_demo_feed(handle, UInt32(clamping: port), $0.baseAddress, $0.count)
        }
    }
    @discardableResult public func demoDisconnect() -> Bool {
        lock.lock(); defer { lock.unlock() }
        return fbv_monitor_demo_disconnect(handle)
    }
    static func now() -> Double {
        var time = timespec()
        clock_gettime(CLOCK_MONOTONIC, &time)
        return Double(time.tv_sec) + Double(time.tv_nsec) / 1e9
    }
}
