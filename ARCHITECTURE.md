# Architecture and development

For download and everyday use, see [README.md](README.md).

## Shared core and source map

The native SwiftUI app and `fbv` CLI link the same C library: one parser,
serialiser, CoreMIDI transport and guarded write transaction. There are no external
packages, GUI subprocesses or duplicated protocol implementations.

- `Sources/FBVCore/include/FBVCore.h`: explicit C session/result/preview API.
- `Sources/FBVCore/engine.c`: parser, protocol, transport, transaction and CLI;
  `Sources/FBVCore/session.h`: public API adapters and offline transport.
- `Sources/FBVCore/monitor.c`, `Sources/FBVCore/include/FBVMonitor.h`: passive input
  lifecycle, device/source resolution, per-source CC parser and bounded queue.
- `Sources/fbv/main.c`: CLI entry; `fbv_read.c`: source compatibility entry only.
- `Sources/PedalKit`: Swift core bridge, main-actor model and SwiftUI UI.
- `Sources/FBVApp`: native app entry and bounded offline render/action smoke.
- `test_fbv_a.c`, `test_monitor.c`, `Tests/PedalKitTests`: core, monitor and model tests.

The pedal illustration is original SwiftUI vector drawing. The icon is original
generated artwork; no third-party reference image is shipped. `Assets/AppIcon.png`
is the 1024px master with transparent margins. The app build uses macOS `sips` and
`iconutil` to generate `.icns`, with intermediate sizes under `.build/`.
Research notes, captured traffic, local backups, build products and snapshots stay
ignored and are not app assets. No licence terms are added here.

## Assignment transaction and protocol limits

Both frontends use the same transaction:

1. Read and validate a fresh full 81-byte dump. GUI apply also compares it with the
   exact snapshot used for preview, refusing unseen device changes.
2. Validate the selected record and values; preserve every unrelated byte.
   No-op returns here, without backup or assignment.
3. Exclusively create a raw original `.syx` backup with mode `0600`, checking write,
   fsync and close. Failure prevents assignment; a failed file may be partial.
4. Read again and compare **all** bytes with the initial fresh dump.
5. Send exactly one selected assignment and require a success acknowledgement.
6. Independently read the full dump and require exact expected bytes and checksum.

The CLI additionally compares its initially displayed snapshot with the
transaction's fresh read. GUI backups live in
`~/Library/Application Support/FBV/Backups/`; CLI backups are
`./fbv-backup-*.syx` in its working directory. The GUI shows backup paths on success
and failure. Demo never creates backups.

There is no restore command, automatic rollback or automatic retry. Any unverified
assignment attempt is **uncertain**, even if sending reported failure. The app
invalidates its snapshot after operational failure; inspect the device and
explicitly refresh before reviewing another change. Raw backups are evidence,
not an invitation to upload whole dumps.

- Configuration access requires exactly one `Port 4` source and one destination
  on the same device named `FBV Express Mk II`. Ambiguous endpoints are refused.
- A–D writes require recognised Single/Toggle/Momentary CC shapes with values
  127/0. Wah and Volume require full-range Continuous CC shapes with values 127/0.
  Unsupported types, ranges, layouts and extra fields are refused, not converted;
  unsupported unselected records are preserved byte-for-byte.
- No arbitrary MIDI/SysEx, toe-switch or global edits, batch changes, whole-preset
  uploads, import/export, firmware commands or range/type editing.
- A–D and Volume assignment/physical behaviour were live-verified before the app
  existed. Wah writes remain offline-tested only. No new live writes were made
  for the app. Full readback does not prove physical behaviour or power-cycle
  persistence. JSON `confirmed_command` denotes a recognised protocol type.
- Avoid other configuration writers. The protocol has no transaction IDs or
  device locks; changes after the final preflight read cannot be locked out.
- Response waits have three-second deadlines. CoreMIDI calls themselves have no
  application timeout. GUI I/O runs off the main thread, with one busy operation.

Selection, draft edits, reset and review send no assignments. The normal app reads
on launch and Refresh, never applying automatically. Selection changes and refresh
ask before discarding a dirty draft; busy operations lock editing and selection.
Wah and Volume are separate assignment records; selecting one does not change
physical expression mode.

## Passive control monitor

**Test controls** listens without sending MIDI. Starting requires exactly one
online physical device named `FBV Express Mk II`, with one source each named
`Port 1`, `Port 2`, `Port 3`. Port 4 is never monitored.

Matching uses the last loaded assignments, never the draft, and compares port,
channel and CC. A–D flash for each matching event, including value 0; this is not
held-switch state. Expression meters show the latest matching value, 0–127.
Shared assignments light together because MIDI does not identify the physical
control or expression mode. The last raw CC stays visible even without a match.
Input never changes selection or drafts.

The parser keeps state per source, handling fragmented messages, running status,
realtime/system messages and SysEx. A bounded queue keeps the newest 256 CCs;
skipped events are counted, not logged. Testing stops and clears activity on
refresh, apply or window teardown. MIDI topology/property/I/O changes invalidate
listening conservatively, including changes unrelated to the FBV. Check USB, then
restart testing or refresh.

## CLI reference

The downloaded executable is `./fbv`; local builds place it at `./dist/fbv`.
Without change fields, it reads the full configuration. With change fields, it
previews one selected-control change by default. Read and preview request fresh
configuration over MIDI, but send no assignments and create no backups.

```sh
./fbv --json
./fbv --pedal B --cc 21 --mode momentary --json
./fbv --pedal volume --cc 95 --json
./fbv --pedal B --cc 21 --mode momentary --apply --json
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
before MIDI access. Self-test cannot include changes, apply or a selector. Help
and self-test never access MIDI. Self-test JSON is a built-in fixture, not a
hardware reading. No implicit prompts, configuration files or environment
overrides. Explicit `--apply` is the CLI's write authorisation.

Exit 0: success, preview or no-op. Exit 2: invalid options. Exit 1: operational
failure, including uncertain writes. Primary output is stdout; diagnostics are
stderr. Read JSON contains raw bytes and supported per-record fields. Proposal
JSON contains status, original/expected dumps, exact assignment and backup path.
Operational error JSON has `status: error`, numeric core `code` and `message`;
uncertain writes retain the full proposal with `status: uncertain`.

## Build and offline verification

Requires macOS 13+ and Xcode / Swift 5.9 or newer. Tested with Swift 6.4.2 on Apple
silicon. Builds target the host architecture; there are no dependencies to install.

```sh
make test app                     # offline checks, release CLI and local app
./dist/fbv --help
open dist/FBV.app --args --demo   # offline playground, never opens MIDI
open dist/FBV.app                 # opens MIDI and reads on launch
make smoke                       # debug native render/action smoke only
./dist/FBV.app/Contents/MacOS/FBVApp --demo --snapshot "$PWD/capture/app-review/release-smoke"
```

`make build` builds debug executables; `make app` builds `dist/fbv` and
`dist/FBV.app`, ad-hoc signed for local use, not notarised or packaged. Quit an
existing app before switching between demo and real mode. The **DEMO · NO MIDI**
badge stays visible in demo. The ignored historical `fbv_read` binary is not
overwritten by these builds.

`make test` includes warning-clean C and Swift builds, the original 428 parser /
38,501 writer checks against the actual library source, AddressSanitizer and
UndefinedBehaviorSanitizer (macOS leak detection disabled), public API boundary /
fault tests, CLI checks and Swift model tests. Model coverage includes drafts,
selection, validation, review, apply, no-op, stale snapshots, preflight drift,
unsupported/disconnected and uncertain paths.

Monitor tests exercise the production parser/queue and callback path offline:
fragmentation, running status, realtime/system/SysEx handling, source isolation,
CC 0, collisions, queue limits, stop/restart and late callbacks. The check script
also runs monitor tests under ThreadSanitizer. Injection is refused on real
monitors. Hardware reception/disconnect still needs live checking.

`--snapshot DIRECTORY` creates its **own demo session**, even without `--demo`,
drives the same model actions as the views, renders four NSHostingView snapshots
including monitor activity and both expression meters, and writes `actions.json`
before exiting. It also renders its own native window frames and confirmation /
discard sheets under inherited light and dark appearance, checking native controls
and compact layout. It uses the native hidden-titlebar configuration, but does not
exercise physical dragging or keyboard input. It never opens MIDI or operates
other apps/windows. Output under `capture/` is local evidence ignored by Git.

Accessibility labels, selection and assignment values, contextual help and keyboard
metadata are present. Tab / Shift-Tab use macOS Keyboard Navigation; ⌘⌥A/B/C/D/W/V
select controls and ⌘R refreshes. Automated VoiceOver or physical keyboard
interaction has not been performed. Native snapshot checks are not end-to-end
accessibility or hardware verification.

## Signed and notarised releases

Version 1.0.0, build 1 uses bundle ID `com.mixitone.fbv`. `make app` remains
ad-hoc signed. Distribution requires an arm64 Mac, Apple command-line tools,
`op` (authenticated 1Password CLI), `jq` and a Developer ID Application P12.
The secret item must contain `username`, `APPLE_CERTIFICATE_PASSWORD`,
`APPLE_PASSWORD` (app-specific password) and `APPLE_TEAM_ID`.

```sh
FBV_SIGNING_P12=/path/to/DeveloperID.p12 \
FBV_NOTARY_SECRET_ITEM=op://YourVault/YourSigningItem make notarize
make test-release                # offline packaging checks with fake signing tools
```

The release script builds both executables, signs them with hardened runtime in a
temporary keychain, verifies the configured team, submits once, then staples and
checks the app with Gatekeeper. It never changes the user keychain search list.
`dist/releases/v1.0.0-arm64/` contains `FBV.app`, the standalone signed `fbv`,
`FBV-1.0.0-arm64.zip` (both) and `SHA256SUMS`. This is arm64 only, not universal.
The CLI relies on the archive's notarisation; only app bundles can be stapled.
No installation, GitHub publication or tagging is performed by the script.

Existing release output is never overwritten. Private build/submission evidence
stays in ignored `dist/releases/.notarize-*` directories, including on failure;
only the temporary keychain and owned lock are removed automatically. Upload and
polling are separate: the submission ID is saved before waiting. Redacted
diagnostics and command exit statuses are retained.

After upload or wait failure, inspect the saved submission ID and query that
submission rather than blindly resubmitting. If there is no valid ID, inspect the
private evidence before any retry. Stapling or packaging failures retain the
signed payload for manual recovery. Inspect a stale `.v1.0.0-arm64.lock` after an
uncatchable kill before removing it. Keep evidence private and remove it manually
when no longer needed. Never commit credentials or run the script with tracing.
