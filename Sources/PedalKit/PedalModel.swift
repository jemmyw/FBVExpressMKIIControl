import Foundation
import Combine

public struct Confirmation: Identifiable, Sendable {
    public let id = UUID()
    public let control: Control
    public let before: Assignment
    public let after: Assignment
    public let snapshot: [UInt8]
    public let sysex: String
}

public enum DiscardIntent: Equatable {
    case select(Control), refresh
}

@MainActor public final class PedalModel: ObservableObject {
    public let session: CoreSession
    @Published public private(set) var selected: Control = .a
    @Published public private(set) var snapshot: [UInt8]?
    @Published public var ccText = ""
    @Published public var port = 1
    @Published public var channel = 1
    @Published public var mode = 3
    @Published public private(set) var busy = false
    @Published public private(set) var confirmation: Confirmation?
    @Published public private(set) var discardIntent: DiscardIntent?
    @Published public private(set) var message = "Not connected"
    @Published public private(set) var backup = ""
    @Published public private(set) var lastStatus: Status?

    @Published public private(set) var inputState: InputState = .stopped
    @Published public private(set) var lastInput: InputEvent?
    @Published public private(set) var inputActivity: [Control: InputEvent] = [:]
    @Published public private(set) var inputValues: [Control: Int] = [:]
    @Published public private(set) var droppedInput: UInt64 = 0
    private var inputTask: Task<Void, Never>?
    private var inputGeneration = 0

    public var inputMessage: String {
        switch inputState {
        case .stopped: return ""
        case .listening: return lastInput?.detail ?? ""
        case .disconnected: return "Input unavailable. Check USB, then restart testing or refresh."
        }
    }
    public func startTesting() async {
        guard !locked, snapshot != nil, inputState != .listening else { return }
        stopTesting()
        let generation = inputGeneration
        busy = true
        let monitor = session.inputMonitor
        let started = await Task.detached { monitor.start() }.value
        busy = false
        guard generation == inputGeneration else { monitor.stop(); return }
        inputState = started ? .listening : .disconnected
        if !started { message = "Input unavailable. Check USB and refresh." }
        else { message = session.demo ? "Demo ready" : "Input listening" }
        guard started else { return }
        inputTask = Task { @MainActor [weak self] in
            while !Task.isCancelled {
                do { try await Task.sleep(nanoseconds: 50_000_000) } catch { return }
                guard let self else { return }
                self.pollInput()
                if self.inputState != .listening { return }
            }
        }
    }
    public func stopTesting() {
        inputGeneration += 1
        inputTask?.cancel(); inputTask = nil
        session.inputMonitor.stop()
        if message == "Input listening" { message = "Configuration loaded" }
        inputState = .stopped; lastInput = nil; inputActivity = [:]; inputValues = [:]; droppedInput = 0
    }
    public func pollInput() {
        guard inputState == .listening else { return }
        let (state, events, dropped) = session.inputMonitor.drain()
        inputState = state; droppedInput = dropped
        guard state == .listening else {
            message = "Input unavailable. Check USB and refresh."
            lastInput = nil; inputActivity = [:]; inputValues = [:]
            return
        }
        let now = InputMonitor.now()
        inputActivity = inputActivity.filter { now - $0.value.received < 0.25 }
        for event in events {
            lastInput = event
            for control in Control.allCases {
                if let assignment = assignment(for: control), assignment.port == event.port,
                   assignment.channel == event.channel, assignment.cc == event.cc {
                    inputValues[control] = event.value
                    if now - event.received < 0.25 { inputActivity[control] = event }
                }
            }
        }
    }
    deinit { inputTask?.cancel(); session.inputMonitor.stop() }

    public init(session: CoreSession) {
        self.session = session
        if session.demo { message = "Demo · no MIDI" }
    }
    public var locked: Bool { busy || confirmation != nil || discardIntent != nil }
    public var loaded: Assignment? {
        guard let snapshot else { return nil }
        return session.assignment(snapshot: snapshot, control: selected)
    }
    public var draft: Assignment? {
        guard !ccText.isEmpty, ccText.utf8.allSatisfy({ $0 >= 48 && $0 <= 57 }),
              let cc = Int(ccText), (0...127).contains(cc), (1...3).contains(port),
              (1...16).contains(channel), selected.isExpression || (1...3).contains(mode) else { return nil }
        return Assignment(cc: cc, port: port, channel: channel, mode: selected.isExpression ? -1 : mode)
    }
    public var dirty: Bool { loaded != nil && (draft != loaded || ccText != String(loaded!.cc)) }
    public var preview: CoreResult? {
        guard let snapshot, let draft, loaded != nil else { return nil }
        return session.preview(snapshot: snapshot, control: selected, draft: draft)
    }
    public var canReview: Bool { !locked && preview?.status == .preview }
    public var validation: String? {
        guard loaded != nil else { return nil }
        return draft == nil ? "Use a whole CC number from 0 to 127, port 1–3 and channel 1–16." : nil
    }
    public func assignment(for control: Control) -> Assignment? {
        guard let snapshot else { return nil }
        return session.assignment(snapshot: snapshot, control: control)
    }
    private func resetDraft() {
        guard let value = loaded else { ccText = ""; return }
        ccText = String(value.cc); port = value.port; channel = value.channel
        mode = value.mode == -1 ? 3 : value.mode
    }
    public func discardDraft() {
        guard !locked else { return }
        resetDraft()
    }
    public func select(_ control: Control) {
        guard !locked, control != selected else { return }
        if dirty { discardIntent = .select(control); return }
        selected = control; resetDraft()
    }
    public func requestRefresh() async {
        guard !locked else { return }
        if dirty { discardIntent = .refresh; return }
        await refresh()
    }
    public func cancelDiscard() { guard !busy else { return }; discardIntent = nil }
    public func resolveDiscard(_ discard: Bool) async {
        guard !busy, let intent = discardIntent else { return }
        discardIntent = nil
        guard discard else { return }
        switch intent {
        case .select(let control): selected = control; resetDraft()
        case .refresh: await refresh()
        }
    }
    private func refresh() async {
        guard !locked else { return }
        stopTesting()
        busy = true; message = "Reading the pedal…"
        let session = session
        let result = await Task.detached { session.read() }.value
        lastStatus = result.status; backup = ""
        if result.status == .read {
            snapshot = result.dump; resetDraft()
            message = session.demo ? "Demo ready" : "Connected"
        } else {
            snapshot = nil; resetDraft(); message = result.message
        }
        busy = false
    }
    public func review() {
        guard canReview, let loaded, let draft, let snapshot, let preview else { return }
        confirmation = Confirmation(control: selected, before: loaded, after: draft,
                                    snapshot: snapshot, sysex: preview.sysex)
    }
    public func cancelReview() { guard !busy else { return }; confirmation = nil }
    public func applyConfirmed() async {
        guard !busy, let review = confirmation else { return }
        stopTesting()
        busy = true; confirmation = nil; message = "Applying and verifying…"
        let session = session
        let result = await Task.detached {
            session.apply(snapshot: review.snapshot, control: review.control, draft: review.after)
        }.value
        lastStatus = result.status; backup = result.backup
        switch result.status {
        case .applied, .noOp:
            snapshot = result.expected; resetDraft()
            message = result.status == .noOp ? "No changes needed" :
                session.demo ? "Demo updated · no MIDI sent" : "\(review.control.title) updated and verified"
        default:
            snapshot = nil; resetDraft(); message = result.message
        }
        busy = false
    }
}
