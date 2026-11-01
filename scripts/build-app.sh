#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
swift build -c release -Xcc -Wall -Xcc -Wextra -Xcc -Wpedantic -Xcc -Werror -Xswiftc -warnings-as-errors
bin=$(swift build -c release --show-bin-path)
iconset=.build/AppIcon.iconset
mkdir -p "$iconset" dist/FBV.app/Contents/MacOS dist/FBV.app/Contents/Resources
for size in 16 32 128 256 512; do
    sips -z "$size" "$size" Assets/AppIcon.png --out "$iconset/icon_${size}x${size}.png" >/dev/null
    double=$((size * 2))
    sips -z "$double" "$double" Assets/AppIcon.png --out "$iconset/icon_${size}x${size}@2x.png" >/dev/null
done
iconutil -c icns "$iconset" -o dist/FBV.app/Contents/Resources/AppIcon.icns
cp "$bin/FBVApp" dist/FBV.app/Contents/MacOS/FBVApp
cp "$bin/fbv" dist/fbv
cp scripts/Info.plist dist/FBV.app/Contents/Info.plist
cp LICENSE dist/FBV.app/Contents/Resources/LICENSE
codesign --force --sign - dist/FBV.app
printf '\nBuilt: dist/fbv and dist/FBV.app\nDemo: open dist/FBV.app --args --demo\n'
