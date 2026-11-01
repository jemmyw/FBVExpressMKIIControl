# FBV assignment studio

A native macOS SwiftUI app and `fbv` command-line utility for programming supported
MIDI CC assignments on the Line 6 FBV Express Mk II. Both link the same C library:
one parser, serializer, CoreMIDI transport and guarded write transaction. No external
packages, GUI subprocesses or duplicated protocol implementation.

## Build, test and launch

Requires macOS 13+ and Xcode / Swift 5.9 or newer. Tested with Swift 6.4.2 on Apple
silicon. Builds for the host architecture; no dependencies to install.

```sh
make test app                     # offline checks + release CLI and signed local app
./dist/fbv --help
open dist/FBV.app --args --demo   # offline playground, never opens MIDI
open dist/FBV.app                 # real device: opens MIDI and reads on launch
```

Quit an existing app before switching between demo and real mode. The app's
**DEMO · NO MIDI** badge is always visible in demo. Selection, editing, resetting
a draft and reviewing a change send no assignments. The normal app reads on launch
and when you choose Refresh. It never applies automatically.

`make build` builds debug executables; `make app` builds `dist/fbv` and
`dist/FBV.app`. The app is ad-hoc signed for local use, not notarised or packaged
for distribution. No installation is required: keep the app wherever you like;
remove the app and CLI to uninstall. Backups are kept separately and not removed.
The ignored historical `fbv_read` binary is not overwritten.

`Assets/AppIcon.png` is the original 1024px icon master with transparent margins.
`make app` generates the bundled `.icns` using macOS `sips` and `iconutil`, with
intermediate sizes under `.build/`. No image-editing tools are needed to build.

### In the app

- Click A–D or the treadle's labelled Wah / Volume areas to choose an assignment.
  The physical layout is C D above A B. A checkmark and outline identify selection.
- Edit CC, output port and channel. A–D also offer Single, Toggle and Momentary.
  Expression assignments remain full-range Continuous CC.
- Preview is local and uses the shared core. **Review change…** shows the selected
  control and before/after fields. Only the explicit confirmation applies it.
- **Test controls** listens on ordinary input Ports 1–3 without sending MIDI.
  A–D flash briefly on each matching CC, including value 0; this is not held-switch
  state. Wah / Volume show the latest matching value, 0–127. Matching uses the last
  loaded assignments, never the draft. Shared assignments light together because
  MIDI does not identify which physical mode produced them. The last raw CC stays
  visible even when no assignment matches. No input changes selection or drafts.
- Testing stops and clears activity on refresh, apply or window teardown. MIDI
  topology/property/I/O changes invalidate listening conservatively, including
  changes unrelated to the FBV. Check USB, then restart testing or refresh. Starting
  requires exactly one online physical device named `FBV Express Mk II` with one
  source each named `Port 1`, `Port 2`, `Port 3`. Port 4 is never monitored. A bounded
  queue keeps the newest 256 CCs; skipped events are counted, not logged.
- **Reset draft** restores the last reading. Changing selection or refreshing with
  a dirty draft asks before discarding it. Busy operations lock editing/selection.
- Use Tab / Shift-Tab with macOS Keyboard Navigation enabled, or ⌘⌥A/B/C/D/W/V to
  select controls. ⌘R refreshes. Buttons expose labels, selection and assignment
  values to accessibility tools and have contextual help.

Wah and Volume are separate assignment records. Selecting either in the app does
**not** change the physical expression mode. Pressing the physical toe switch
selects Wah / Volume. The toe switch itself has no programmable scope here.
The in-app pedal is original SwiftUI vector drawing. The app icon is original
generated artwork; no third-party reference image is shipped.

## Command line

Without change fields, read the full configuration. With change fields, preview
one selected-control change by default. Reading and previewing request fresh
configuration over MIDI, but send no assignments and create no backups.

```sh
./dist/fbv --json
./dist/fbv --pedal B --cc 21 --mode momentary --json
./dist/fbv --pedal A --port 1 --channel 1 --cc 20 --mode momentary
./dist/fbv --pedal volume --cc 95 --json
./dist/fbv --pedal B --cc 21 --mode momentary --apply --json
```

| Option | Values |
| --- | --- |
| `--pedal` | `A`, `B`, `C`, `D`, `wah`, `volume`; default `A` |
| `--cc` | 0–127 |
| `--port` | 1–3 |
| `--channel` | 1–16 |
| `--mode` | `single`, `toggle`, `momentary`; A–D only |
| `--apply` | Explicitly apply one backed-up, drift-checked assignment |
| `--json` | Structured read/proposal/error output |
| `--self-test` | Offline parser tests; may combine with `--json` |
| `--help`, `-h` | Usage without MIDI access |

Each value option may appear once. Omitted fields are preserved. A selector alone
only reads. `--apply` requires a change field; expression `--mode` is rejected
before MIDI access. Help and self-test never access MIDI. Self-test JSON is a
built-in fixture, not a hardware reading. No implicit prompts, configuration files
or environment overrides. Explicit `--apply` is the CLI's write authorisation.

Exit 0: success, preview or no-op. Exit 2: invalid options. Exit 1: operational
failure, including uncertain writes. Primary output is stdout; diagnostics are
stderr. Read JSON contains raw bytes and supported per-record fields. Proposal
JSON contains status, original/expected dumps, exact assignment and backup path.
Operational error JSON has `status: error`, numeric core `code` and `message`;
uncertain writes retain the full proposal with `status: uncertain`.

## Write safety

Both frontends use the same transaction:

1. Read and validate a fresh full 81-byte dump. GUI apply also compares this with
   the exact snapshot used for its preview, refusing unseen device changes.
2. Validate the selected record and requested values; preserve every unrelated byte.
   No-op returns now: no backup and no assignment.
3. Exclusively create a raw original `.syx` backup with mode `0600`, checking write,
   fsync and close. Failure prevents assignment; a failed file may be partial.
4. Read again and compare **all** bytes with the initial fresh dump.
5. Send exactly one selected assignment and require a success acknowledgement.
6. Independently read the full dump and require exact expected bytes and checksum.

GUI backups: `~/Library/Application Support/FBV/Backups/`.
CLI backups: `./fbv-backup-*.syx` in its working directory. The CLI additionally
compares the initial displayed snapshot against its transaction's fresh read.
The GUI shows backup paths on success and failure. Demo never creates backup files.

There is no restore command, automatic rollback or automatic retry. Any unverified
assignment attempt is **uncertain**, even if sending reported failure. The app
invalidates its snapshot after operational failure; inspect the device and
explicitly refresh before reviewing another change. Backup files are raw evidence,
not an invitation to upload whole dumps.

## Supported scope and limitations

- Only FBV Express Mk II: exactly one `Port 4` source and one destination on the
  same named device. Missing or ambiguous endpoints are refused.
- A–D writes require recognised Single/Toggle/Momentary CC shapes with values
  127/0. Wah / Volume require full-range Continuous CC shapes with values 127/0.
  Unsupported types, ranges, layouts and extra fields are refused, not converted.
  Unsupported unselected records are preserved byte-for-byte.
- No arbitrary MIDI/SysEx, toe-switch or global edits, batch changes, whole-preset
  uploads, import/export, firmware commands or range/type editing.
- A–D and Volume assignment/physical behaviour were live-verified before this app
  existed. Wah writes remain offline-tested only. No new live writes were made
  for the app. Full readback does not prove physical behaviour or power-cycle
  persistence. JSON `confirmed_command` denotes a recognised protocol type.
- Avoid other configuration writers. The protocol has no transaction IDs or
  device locks; changes after the final preflight read cannot be locked out.
- Response waits have three-second deadlines. CoreMIDI calls themselves have no
  application timeout. GUI I/O runs off the main thread, with one busy operation.
- Native accessibility and keyboard metadata are present; automated VoiceOver or
  physical keyboard interaction has not been performed. No browser controller
  is used to operate native windows.

## Development and offline verification

```sh
make test                        # no MIDI: C, sanitizers, Swift, CLI, native render/actions
make smoke                       # debug native render/action smoke only
./dist/FBV.app/Contents/MacOS/FBVApp --demo --snapshot "$PWD/capture/app-review/release-smoke"
```

`--snapshot DIRECTORY` always creates its **own demo session**, even without
`--demo`, drives the same model actions as the views, renders four NSHostingView
snapshots, including live monitor activity and both expression meters. It also
renders its own native window frames and confirmation/discard sheets under
inherited light and dark appearance, checking native controls and compact layout.
The window harness uses the native hidden-titlebar configuration; it does not
exercise physical dragging or keyboard input. It writes `actions.json`, then exits.
It never opens MIDI or operates other apps/windows. Output is local evidence,
ignored by Git under `capture/`.
`make test` includes warning-clean C and Swift builds, all original 428 parser /
38,501 writer checks against the actual library source, AddressSanitizer and
UndefinedBehaviorSanitizer (macOS leak detection disabled), public API boundary /
fault tests, and Swift model tests for draft, selection, validation, review, apply,
no-op, stale snapshot, preflight drift, unsupported/disconnected and uncertain paths.
Monitor tests exercise the production parser/queue and callback path offline,
including fragmentation, running status, realtime/system/SysEx handling, source
isolation, CC 0, collisions, queue limits, stop/restart and late callbacks. Injection
is refused on real monitors. Hardware reception/disconnect still needs live checking.

Source map:

- `Sources/FBVCore/include/FBVCore.h`: explicit C session/result/preview API.
- `Sources/FBVCore/engine.c`: established parser, protocol, transport, transaction
  and CLI implementation; `session.h`: public API adapters and offline transport.
- `Sources/FBVCore/monitor.c`, `include/FBVMonitor.h`: passive input lifecycle,
  exact device/source resolution, per-source CC parser and bounded event queue.
- `Sources/fbv/main.c`: CLI entry; `fbv_read.c`: source compatibility entry only.
- `Sources/PedalKit`: Swift core bridge, main-actor model and original SwiftUI UI.
- `Sources/FBVApp`: native app entry and bounded offline render/action smoke.
- `test_fbv_a.c`, `Tests/PedalKitTests`: regression, core API and model tests.

Research notes, captured traffic, existing local backups, build products and
snapshots remain ignored and are not app assets. No licence terms are added.
