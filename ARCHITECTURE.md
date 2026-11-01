# Architecture

For downloads and everyday use, see [README.md](README.md).

## Components and shared core

The SwiftUI app and `fbv` CLI link the same C library. Parsing, serialisation,
CoreMIDI transport and guarded writes have one implementation, with no external
packages or GUI subprocesses.

- `Sources/FBVApp`: app entry and session/model ownership.
- `Sources/PedalKit`: SwiftUI views, main-actor `PedalModel`, Swift value types and
  `CoreSession`/`InputMonitor` bridges to C.
- `Sources/FBVCore/include/FBVCore.h`: opaque session API and value-based results;
  `engine.c`: configuration parser, protocol, transport, transaction and CLI;
  `session.h`: public API adapters and offline transport.
- `Sources/FBVCore/monitor.c` and `include/FBVMonitor.h`: passive input lifecycle,
  source resolution, per-source CC parsing and bounded event queue.
- `Sources/fbv/main.c`: thin CLI entry into the shared core.

## State and data flow

A read produces a validated raw snapshot. The model derives loaded assignments
from it and keeps draft fields separate. Preview is a pure transformation of the
snapshot into expected bytes and one assignment message; it does not access MIDI.
Review freezes the selected control, before/after values and exact snapshot.
Only explicit confirmation applies that proposal. The CLI likewise defaults to
read/preview and requires `--apply` to authorise a write.

The app reads on launch and Refresh. Selection, draft edits, reset and review
never send assignments. Dirty drafts require confirmation before selection or
refresh discards them. Busy operations lock editing and selection. Success replaces
the snapshot with verified expected bytes; apply failure invalidates it, requiring
an explicit refresh before another review. Demo uses an in-memory transport,
without creating MIDI objects or backup files.

## Assignment transaction and protocol limits

Both frontends use the same guarded transaction:

1. Read and validate a fresh full 81-byte dump, comparing it with the exact
   baseline snapshot used for the proposal. Refuse drift before creating a backup.
2. Validate the selected record and requested values. Preserve unrelated bytes
   and recompute the checksum. A no-op returns without backup or assignment.
3. Exclusively create the raw original `.syx` backup with mode `0600`; require
   successful write, fsync and close. Failure prevents assignment but may leave a
   partial backup file.
4. Read again and compare every byte with the initial fresh dump.
5. Attempt exactly one 21-byte selected assignment and require a success ACK.
6. Independently read the full dump and require valid checksum and exact expected
   bytes, even if sending or acknowledgement failed.

Any assignment attempt without both ACK and exact readback is **uncertain**.
There is no automatic retry, rollback or restore command. Backups are evidence,
not whole-dump upload instructions. The protocol has no transaction IDs or device
locks, so another writer can still race after the final preflight read.

A–D writes require recognised Single/Toggle/Momentary CC records with 127/0
values; Wah and Volume require full-range Continuous CC records with 127/0 values.
Unknown layouts, types, ranges and extra fields are refused, not converted.
Unsupported unselected records are preserved byte-for-byte. There are no arbitrary
MIDI/SysEx, toe-switch/global edits, batch writes, preset uploads, import/export,
firmware commands or range/type edits. Wah writes remain offline-tested only.
Readback cannot prove physical behaviour or power-cycle persistence. Wah and
Volume are separate records; selection does not change physical expression mode.

## Configuration and passive input routing

Configuration requires exactly one `Port 4` source and one destination on the same
device named `FBV Express Mk II`; ambiguous endpoints are refused. Its parser
assembles fragmented SysEx, ignores realtime bytes, rejects malformed frames and
accepts only the expected dump or ACK. Response waits have three-second deadlines;
CoreMIDI calls themselves have no application timeout.

The passive monitor sends no MIDI. It requires exactly one online physical device
named `FBV Express Mk II`, with one source each named `Port 1`, `Port 2`, `Port 3`.
It never monitors Port 4. Parser state is independent per source: fragments and
running status survive packet boundaries, realtime bytes are ignored, and system
messages/SysEx clear running status. Only complete CC messages enter the queue.
The queue retains the newest 256 events, dropping the oldest and counting losses.

The model drains events and matches port/channel/CC against loaded assignments,
never drafts. A–D activity includes value 0 and is not held-switch state; expression
meters retain the latest value. Colliding assignments light together because MIDI
cannot identify the physical control or expression mode. Input never edits drafts
or selection. Refresh, apply and window teardown stop testing and clear activity.
Topology/property/I/O notifications invalidate listening conservatively, even for
unrelated MIDI changes; restarting requires explicit action.

## Concurrency and lifetime

The main-actor model runs blocking configuration I/O off the main thread, with one
busy operation. `CoreSession` locks session operations; C callers must serialise
session use and destruction. Monitor public calls must also be serialised by their
owner; callbacks update internal state and queues, never call the UI.

Configuration and monitor callbacks use opaque integer tokens, never dereferenced
pointers. Process-lifetime registries protect lookup and callback access with
mutexes. Tokens never repeat; exhaustion refuses a new registration. Configuration
lock order is registry then reader. Teardown removes the registration under lock,
waiting out callbacks before destroying reader state, then disposes CoreMIDI
objects outside the lock. Late callbacks cannot reach freed or reused storage.
Configuration channels are opened and closed per operation. Disposing the last
CoreMIDI client has an SDK-documented recreation risk if the MIDI server exits.

## Build and checks

On macOS 13+ with Xcode / Swift 5.9+: `make build` builds debug executables,
`make app` builds the local app and CLI in `dist/`, and `make test` runs offline
C/Swift checks, sanitisers and native smoke checks without opening hardware.
