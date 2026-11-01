import SwiftUI

private let ink = Color(red: 0.14, green: 0.17, blue: 0.18)
private let cream = Color(red: 0.97, green: 0.95, blue: 0.89)
private let teal = Color(red: 0.08, green: 0.39, blue: 0.37)
private let coral = Color(red: 0.96, green: 0.47, blue: 0.35)

public struct PedalView: View {
    @ObservedObject var model: PedalModel
    public init(model: PedalModel) { self.model = model }

    public var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            HStack(alignment: .top) {
                Text("FBV Express Mk II")
                    .font(.system(size: 28, weight: .bold, design: .rounded))
                Spacer()
                if model.session.demo {
                    Label("DEMO · NO MIDI", systemImage: "sparkles")
                        .font(.system(size: 11, weight: .bold, design: .monospaced))
                        .padding(10).background(coral.opacity(0.2), in: Capsule())
                        .help("Offline playground. This session never opens MIDI or writes backup files.")
                }
            }
            HStack(alignment: .top, spacing: 28) {
                VStack(alignment: .leading, spacing: 16) {
                    PedalIllustration(model: model)
                    inputControls
                    Spacer(minLength: 0)
                }.frame(maxWidth: .infinity)
                editor.frame(width: 294)
            }
            Spacer(minLength: 0)
            status
        }
        .padding(28).background(cream.ignoresSafeArea()).foregroundStyle(ink)
        .frame(minWidth: 1000, idealWidth: 1040, minHeight: 600, idealHeight: 600)
        .preferredColorScheme(.light)
        .onDisappear { model.stopTesting() }
        .sheet(isPresented: Binding(
            get: { model.confirmation != nil || model.discardIntent != nil },
            set: { if !$0 { model.cancelReview(); model.cancelDiscard() } }
        )) {
            VStack(alignment: .leading, spacing: 20) {
                if let review = model.confirmation {
                    Text("Apply \(review.control.title) assignment?").font(.title2.bold())
                    VStack(alignment: .leading, spacing: 12) {
                        Text("BEFORE").font(.caption.bold()).foregroundStyle(ink.opacity(0.65))
                        Text(review.before.summary).font(.system(size: 14, design: .monospaced))
                        Divider()
                        Text("AFTER").font(.caption.bold()).foregroundStyle(teal)
                        Text(review.after.summary).font(.system(size: 14, design: .monospaced))
                    }.padding(16).background(.white.opacity(0.8), in: RoundedRectangle(cornerRadius: 12))
                    Text(model.session.demo ? "Demo only. No MIDI sent." : "Backs up and verifies this change. No automatic retry or rollback.")
                        .font(.system(size: 13)).fixedSize(horizontal: false, vertical: true)
                    HStack {
                        Button("Keep editing") { model.cancelReview() }.keyboardShortcut(.cancelAction)
                        Spacer()
                        Button(model.session.demo ? "Apply in demo" : "Apply to pedal") {
                            Task { await model.applyConfirmed() }
                        }.buttonStyle(.borderedProminent).tint(teal).keyboardShortcut(.defaultAction)
                    }
                } else {
                    Text("Discard draft?").font(.title2.bold())
                    HStack {
                        Button("Keep editing") { model.cancelDiscard() }.keyboardShortcut(.cancelAction)
                        Spacer()
                        Button("Discard draft", role: .destructive) { Task { await model.resolveDiscard(true) } }
                    }
                }
            }.padding(28).frame(width: 520).background(cream).foregroundStyle(ink)
                .preferredColorScheme(.light)
        }
    }

    private var inputControls: some View {
        VStack(alignment: .leading, spacing: 6) {
            HStack {
                Toggle("Test controls", isOn: Binding(
                    get: { model.inputState == .listening },
                    set: { enabled in
                        if enabled { Task { await model.startTesting() } }
                        else { model.stopTesting() }
                    }
                )).toggleStyle(.switch).fixedSize()
                    .disabled(model.locked || model.snapshot == nil)
                    .help("Listen only. Flashes mark received CC events, not held switches. Shared assignments light together.")
                Spacer()
                if model.droppedInput > 0 { Text("\(model.droppedInput) skipped").font(.caption) }
            }
            if !model.inputMessage.isEmpty {
                Text(model.inputMessage).font(.system(size: 11, design: .monospaced))
                    .foregroundStyle(ink.opacity(0.8)).fixedSize(horizontal: false, vertical: true)
                    .accessibilityLabel("Input: \(model.inputMessage)")
            }
        }
    }

    private var editor: some View {
        VStack(alignment: .leading, spacing: 17) {
            HStack(alignment: .firstTextBaseline) {
                Text(model.selected.title).font(.system(size: 30, weight: .bold, design: .rounded))
                Spacer()
                if model.dirty {
                    Text("UNSAVED")
                        .font(.system(size: 10, weight: .bold, design: .monospaced))
                        .foregroundStyle(teal)
                }
            }
            if model.loaded != nil {
                if model.selected.isExpression {
                    Text("Continuous · 0–127").font(.caption).foregroundStyle(ink.opacity(0.7))
                }
                VStack(alignment: .leading, spacing: 14) {
                    VStack(alignment: .leading, spacing: 6) {
                        Text("CC number").font(.system(size: 13, weight: .semibold))
                        HStack(spacing: 6) {
                            TextField("0–127", text: $model.ccText).textFieldStyle(.roundedBorder)
                                .accessibilityLabel("Controller number, 0 to 127")
                                .help("MIDI CC number, from 0 to 127.")
                            Stepper("CC number", value: Binding(
                                get: { model.draft?.cc ?? model.loaded?.cc ?? 0 },
                                set: { model.ccText = String($0) }
                            ), in: 0...127)
                                .labelsHidden().fixedSize()
                                .disabled(model.draft == nil)
                                .accessibilityLabel("Adjust controller number")
                        }
                    }
                    Picker("MIDI port", selection: $model.port) {
                        ForEach(1...3, id: \.self) { Text("Port \($0)").tag($0) }
                    }.help("Output port for this control, from 1 to 3.")
                    Picker("Channel", selection: $model.channel) {
                        ForEach(1...16, id: \.self) { Text("\($0)").tag($0) }
                    }.help("MIDI channel, from 1 to 16.")
                    if !model.selected.isExpression {
                        Picker("Switch mode", selection: $model.mode) {
                            Text("Single").tag(1); Text("Toggle").tag(2); Text("Momentary").tag(3)
                        }.help("Supported CC modes only. Momentary sends 127 on press and 0 on release.")
                    }
                }.disabled(model.locked)
                if let error = model.validation {
                    Label(error, systemImage: "exclamationmark.triangle")
                        .font(.caption).foregroundStyle(Color(red: 0.65, green: 0.2, blue: 0.12))
                }
                Divider()
                VStack(alignment: .leading, spacing: 7) {
                    Text("PREVIEW").font(.system(size: 10, weight: .bold, design: .monospaced))
                    if let preview = model.preview {
                        Text(preview.status == .noOp ? "No changes" : "Not applied")
                            .font(.system(size: 13, weight: .semibold))
                        DisclosureGroup("Assignment bytes") {
                            Text(preview.sysex).font(.system(size: 10, design: .monospaced))
                                .textSelection(.enabled).padding(.top, 6)
                        }.font(.caption)
                    } else { Text("Check your values to preview.").font(.caption) }
                }
                HStack {
                    Button("Reset draft") { model.discardDraft() }
                        .disabled(!model.dirty || model.locked)
                    Spacer()
                    Button("Review change…") { model.review() }
                        .buttonStyle(.borderedProminent).tint(teal).disabled(!model.canReview)
                }.controlSize(.large)
            } else {
                Image(systemName: model.snapshot == nil ? "cable.connector" : "exclamationmark.shield")
                    .font(.system(size: 30)).foregroundStyle(teal).padding(.top, 10)
                Text(model.snapshot == nil ? "No pedal loaded" : "Unsupported assignment")
                    .font(.headline)
                Text(model.snapshot == nil ? "Connect via USB, then refresh." : "Read-only. Only supported CC assignments can be edited.")
                    .font(.system(size: 13)).fixedSize(horizontal: false, vertical: true)
                if model.session.demo { Text("Refresh to load demo assignments.").font(.caption) }
            }
        }.padding(20).background(.white.opacity(0.75), in: RoundedRectangle(cornerRadius: 20))
    }

    private var status: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack(spacing: 12) {
                if model.busy { ProgressView().controlSize(.small) }
                else { Image(systemName: model.lastStatus == .uncertain ? "exclamationmark.triangle.fill" : "waveform.path") }
                Text(model.message).font(.system(size: 13)).fixedSize(horizontal: false, vertical: true)
                    .accessibilityLabel("Status: \(model.message)")
                Spacer(minLength: 16)
                Button { Task { await model.requestRefresh() } } label: {
                    Label("Refresh", systemImage: "arrow.clockwise")
                }
                    .disabled(model.locked).keyboardShortcut("r", modifiers: .command)
                    .help("Read fresh configuration. Unsaved edits require a discard confirmation.")
            }
            if !model.backup.isEmpty {
                Text("Raw backup: \(model.backup)").font(.system(size: 11, design: .monospaced))
                    .textSelection(.enabled)
            }
        }.padding(16).frame(maxWidth: .infinity, alignment: .leading)
            .background(teal.opacity(0.08), in: RoundedRectangle(cornerRadius: 14))
    }
}

private struct PedalIllustration: View {
    @ObservedObject var model: PedalModel
    var body: some View {
        VStack(spacing: 12) {
            HStack(alignment: .top) {
                VStack(alignment: .leading, spacing: 4) {
                    Text("FBV").font(.system(size: 24, weight: .black, design: .rounded)).italic()
                    Text("EXPRESS / MK II").font(.system(size: 9, weight: .bold, design: .monospaced))
                }.foregroundStyle(cream)
                Spacer()
                HStack(spacing: 5) {
                    ForEach(0..<5) { index in
                        Capsule().fill(index == 2 ? coral : cream.opacity(0.25)).frame(width: 15, height: 5)
                    }
                }.padding(.top, 14).accessibilityHidden(true)
            }
            HStack(spacing: 22) {
                VStack(spacing: 18) {
                    HStack(spacing: 18) { control(.c); control(.d) }
                    HStack(spacing: 18) { control(.a); control(.b) }
                }
                VStack(spacing: 6) {
                    Text("EXPRESSION").font(.system(size: 9, weight: .bold, design: .monospaced)).foregroundStyle(cream.opacity(0.8))
                    control(.wah)
                    ForEach(0..<5) { _ in
                        Capsule().fill(cream.opacity(0.12)).frame(height: 3)
                    }.accessibilityHidden(true)
                    control(.volume)
                }.frame(width: 148).padding(12).background(Color.black.opacity(0.35), in: RoundedRectangle(cornerRadius: 16))
                    .overlay(RoundedRectangle(cornerRadius: 16).stroke(cream.opacity(0.35), lineWidth: 2))
            }
            HStack {
                Circle().fill(cream.opacity(0.3)).frame(width: 5, height: 5)
                Spacer()
                Circle().fill(cream.opacity(0.3)).frame(width: 5, height: 5)
            }.accessibilityHidden(true)
        }.padding(22)
            .background(LinearGradient(colors: [Color(red: 0.22, green: 0.25, blue: 0.26), ink], startPoint: .topLeading, endPoint: .bottomTrailing), in: RoundedRectangle(cornerRadius: 26))
            .overlay(RoundedRectangle(cornerRadius: 26).stroke(.black.opacity(0.25), lineWidth: 2))
            .background(RoundedRectangle(cornerRadius: 26).fill(ink.opacity(0.2)).offset(y: 9))
            .padding(.bottom, 9)
    }
    private func control(_ control: Control) -> some View {
        let selected = model.selected == control
        let assignment = model.assignment(for: control)
        return Button { model.select(control) } label: {
            VStack(spacing: 7) {
                HStack(spacing: 4) {
                    if selected { Image(systemName: "checkmark.circle.fill").font(.system(size: 13)) }
                    Text(control.title).font(.system(size: control.isExpression ? 17 : 23, weight: .bold, design: .rounded))
                }
                Text(assignment.map { "CC \($0.cc)" } ?? "···")
                    .font(.system(size: 11, weight: .medium, design: .monospaced))
                if control.isExpression, let value = model.inputValues[control] {
                    HStack(spacing: 5) {
                        ProgressView(value: Double(value), total: 127).tint(coral)
                        Text("\(value)").font(.system(size: 10, design: .monospaced))
                    }.padding(.horizontal, 8)
                }
            }.foregroundStyle(selected ? ink : cream)
                .frame(maxWidth: .infinity).frame(height: control.isExpression ? 65 : 86)
                .background(selected ? cream : Color.white.opacity(0.08), in: RoundedRectangle(cornerRadius: 11))
                .overlay(RoundedRectangle(cornerRadius: 11).stroke(selected ? coral : cream.opacity(0.22), lineWidth: selected ? 3 : 1))
                .overlay(RoundedRectangle(cornerRadius: 11).fill(coral.opacity(model.inputActivity[control] != nil ? 0.55 : 0)))
                .contentShape(RoundedRectangle(cornerRadius: 11))
        }.buttonStyle(.plain).disabled(model.locked)
            .keyboardShortcut(KeyEquivalent(Character(String(control.title.lowercased().prefix(1)))), modifiers: [.command, .option])
            .help("Edit \(control.title) assignment. \(control.isExpression ? "Does not switch physical Wah/Volume mode." : "No MIDI is sent by selection.") Shortcut: ⌘⌥\(control.title.prefix(1)).")
            .accessibilityLabel("\(control.title) assignment")
            .accessibilityValue("\(selected ? "Selected. " : "")\(assignment?.summary ?? "Not loaded or unsupported")\(model.inputValues[control].map { ". Last value \($0)" } ?? "")")
            .accessibilityAddTraits(selected ? [.isSelected] : [])
    }
}
