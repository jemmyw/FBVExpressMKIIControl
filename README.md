# FBV Express Mk II configuration CLI

A native macOS command-line tool for reading and changing supported MIDI CC
assignments on the Line 6 FBV Express Mk II. Written in C with CoreMIDI and
CoreFoundation; no third-party packages required.

A–D assignments and physical press/release behaviour have been live-verified.
Volume reassignment and physical sweep output have also been live-verified.
Wah writes are offline-tested only, not live-verified. Power-cycle persistence
is not guaranteed.

## Build and test

Requires macOS and the Xcode Command Line Tools.

```sh
clang -std=c11 -Wall -Wextra -Wpedantic -Werror -O2 fbv_read.c \
  -framework CoreMIDI -framework CoreFoundation -o fbv_read
./fbv_read --help
./fbv_read --self-test
./fbv_read --self-test --json
clang -std=c11 -Wall -Wextra -Wpedantic -Werror -O2 test_fbv_a.c \
  -framework CoreMIDI -framework CoreFoundation -o /tmp/test-fbv-a
/tmp/test-fbv-a
```

Help and tests never access MIDI. Self-test JSON contains a built-in fixture,
not a hardware reading. Writer tests use fake transport and temporary backups.

## Read and preview

Connect the device over USB. Without change options, the tool reads its current
configuration. With change options, it reads fresh configuration and previews
one selected-control change, including the exact assignment SysEx.

```sh
./fbv_read --json
./fbv_read --pedal B --cc 21 --mode momentary --json
./fbv_read --pedal A --port 1 --channel 1 --cc 20 --mode momentary --json
./fbv_read --pedal wah --json
./fbv_read --pedal volume --cc 95 --json
```

| Option | Values |
| --- | --- |
| `--pedal` | `A`, `B`, `C`, `D`, `wah`, `volume`; default `A` |
| `--cc` | 0–127 |
| `--mode` | `single`, `toggle`, `momentary`; A–D only |
| `--port` | 1–3 |
| `--channel` | 1–16 |
| `--json` | Structured output instead of plain text |

Each value option may appear once. Omitted fields are preserved. A selector
alone only reads. Wah and Volume support CC, port and channel changes only;
`--mode` is rejected for them before MIDI access.

**Preview never sends an assignment or creates a backup.** No-op changes also
send no assignment and create no backup, but still require a fresh read.
Read and preview operations send a configuration read request to the device.

## Apply a change

Review the preview before adding `--apply` to the same options:

```sh
./fbv_read --pedal B --cc 21 --mode momentary --apply --json
```

`--apply` requires at least one change field, not just a selector. For a change,
it:

1. Reads fresh configuration and saves the original full 81-byte raw dump to a
   unique `fbv-backup-*.syx` file in the current directory.
2. Re-reads configuration and refuses to proceed if any byte has changed.
3. Sends one selected-control assignment.
4. Requires a success acknowledgement and an independent full readback matching
   the expected dump, including all unrelated bytes and the checksum.

Backups use exclusive creation, permissions `0600` and checked write/fsync/close.
Backup failure prevents assignment; failed backup files are retained and may be
partial. **There is no restore command, automatic rollback or automatic retry.**
A timeout or verification failure after an assignment attempt reports
`uncertain`. Inspect the device before taking further action; do not retry
uncertain writes automatically.

Invalid options exit 2. Operational errors, including uncertain results, exit 1.
Diagnostics and destination enumeration go to stderr.

## Safety and limitations

- Only the FBV Express Mk II is supported. The tool requires exactly one source
  named `Port 4` on a device named `FBV Express Mk II` and a single destination
  belonging to that device. Ambiguous endpoints are refused.
- Writes require a validated 81-byte configuration dump and a recognised CC
  record shape. A–D require the supported Single/Toggle/Momentary CC shape with
  values 127/0. Wah and Volume require full-range Continuous CC records with
  values 127/0. Unsupported types, modes, ranges or extra fields are refused,
  not converted. Unselected records are preserved byte-for-byte.
- No arbitrary MIDI/SysEx sends, expression range or mode changes, toe-switch or
  global writes, batch changes, whole-preset uploads, preset-file import/export
  or firmware commands are provided.
- Concurrent editor traffic can race reads or acknowledgements. Avoid other
  configuration writers during use. There are no transaction IDs or device
  locks; changes after the final preflight read cannot be locked out.
- Response waits have three-second deadlines, but CoreMIDI calls themselves
  have no application timeout. Full readback verifies configuration bytes, not
  physical control behaviour or power-cycle persistence.
- JSON's `confirmed_command` identifies a recognised protocol type, not live
  verification of each assignment.
