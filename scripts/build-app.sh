#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
swift build -c release -Xcc -Wall -Xcc -Wextra -Xcc -Wpedantic -Xcc -Werror -Xswiftc -warnings-as-errors
bin=$(swift build -c release --show-bin-path)
mkdir -p dist/FBV.app/Contents/MacOS
cp "$bin/FBVApp" dist/FBV.app/Contents/MacOS/FBVApp
cp "$bin/fbv" dist/fbv
cp scripts/Info.plist dist/FBV.app/Contents/Info.plist
codesign --force --sign - dist/FBV.app
printf '\nBuilt: dist/fbv and dist/FBV.app\nDemo: open dist/FBV.app --args --demo\n'
