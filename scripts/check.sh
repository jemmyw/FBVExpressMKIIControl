#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
mkdir -p .build/checks
swift build -Xcc -Wall -Xcc -Wextra -Xcc -Wpedantic -Xcc -Werror -Xswiftc -warnings-as-errors
swift test -Xswiftc -warnings-as-errors
flags='-std=c11 -Wall -Wextra -Wpedantic -Werror -ISources/FBVCore/include'
# Regression tests include the actual library implementation, not a second writer.
clang $flags -O2 test_fbv_a.c -framework CoreMIDI -framework CoreFoundation -o .build/checks/core-tests
.build/checks/core-tests > .build/checks/core.log 2>&1
tail -n 2 .build/checks/core.log
clang $flags -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer test_fbv_a.c \
    -framework CoreMIDI -framework CoreFoundation -o .build/checks/core-tests-sanitized
ASAN_OPTIONS=detect_leaks=0 .build/checks/core-tests-sanitized > .build/checks/sanitizers.log 2>&1
tail -n 2 .build/checks/sanitizers.log
clang $flags -O2 test_monitor.c -framework CoreMIDI -framework CoreFoundation -o .build/checks/monitor-tests
.build/checks/monitor-tests
clang $flags -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer test_monitor.c \
    -framework CoreMIDI -framework CoreFoundation -o .build/checks/monitor-tests-sanitized
ASAN_OPTIONS=detect_leaks=0 .build/checks/monitor-tests-sanitized
clang $flags -g -O1 -fsanitize=thread test_monitor.c \
    -framework CoreMIDI -framework CoreFoundation -o .build/checks/monitor-tests-tsan
.build/checks/monitor-tests-tsan
clang $flags -O2 test_configuration.c -framework CoreMIDI -framework CoreFoundation -o .build/checks/configuration-tests
.build/checks/configuration-tests > .build/checks/configuration.log 2>&1
tail -n 1 .build/checks/configuration.log
clang $flags -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer test_configuration.c \
    -framework CoreMIDI -framework CoreFoundation -o .build/checks/configuration-tests-sanitized
ASAN_OPTIONS=detect_leaks=0 .build/checks/configuration-tests-sanitized > .build/checks/configuration-sanitizers.log 2>&1
tail -n 1 .build/checks/configuration-sanitizers.log
clang $flags -g -O1 -fsanitize=thread test_configuration.c \
    -framework CoreMIDI -framework CoreFoundation -o .build/checks/configuration-tests-tsan
.build/checks/configuration-tests-tsan > .build/checks/configuration-tsan.log 2>&1
tail -n 1 .build/checks/configuration-tsan.log
bin=$(swift build --show-bin-path)
"$bin/fbv" --help > .build/checks/help.txt
"$bin/fbv" --self-test --json > .build/checks/fixture.json
swift -e 'import Foundation; let json = try JSONSerialization.jsonObject(with: Data(contentsOf: URL(fileURLWithPath: CommandLine.arguments[1]))) as! [String: Any]; precondition(json["length"] as? Int == 81)' .build/checks/fixture.json
for args in '--pedal toe' '--cc 128' '--port 0' '--channel 17' '--pedal wah --mode single' '--apply' '--cc 1 --cc 2'; do
    set +e
    "$bin/fbv" $args > .build/checks/invalid.stdout 2> .build/checks/invalid.stderr
    status=$?
    set -e
    test "$status" -eq 2 || { printf 'Invalid arguments did not exit 2: %s\n' "$args"; exit 1; }
done
printf 'CLI help, JSON fixture and seven invalid-input cases passed without MIDI.\n'
"$bin/FBVApp" --snapshot capture/app-review/snapshots
