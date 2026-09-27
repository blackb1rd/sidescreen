#!/bin/zsh
# Build the C++ Spanly.app into dist/ (plus a zip for GitHub releases).
#
#   - universal binary (Apple silicon + Intel), macOS 14 or later; libusb and mbedTLS are
#     linked statically, so the app has no other dependencies
#   - signed with, in order of preference:
#       $SPANLY_SIGN_IDENTITY     e.g. "Developer ID Application: Your Name (TEAMID)"
#       a local self-signed identity ("Spanly Local Signing"): keeps permissions across rebuilds
#       ad-hoc                    Screen Recording / Accessibility must be re-granted after each build
set -euo pipefail

VERSION=${SPANLY_VERSION:-0.1.0}
MIN_MACOS=14.0
ROOT="${0:A:h:h}"
BUILD="$ROOT/build-mac"
DIST="$ROOT/dist"
APP="$DIST/Spanly.app"

echo "==> Building Spanly (universal)"
cmake -S "$ROOT" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" -DCMAKE_OSX_DEPLOYMENT_TARGET=$MIN_MACOS >/dev/null
cmake --build "$BUILD" --target spanly

echo "==> Assembling $APP"
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
cp "$BUILD/spanly" "$APP/Contents/MacOS/spanly"
strip -x "$APP/Contents/MacOS/spanly"
cp "$ROOT/mac/Resources/AppIcon.icns" "$APP/Contents/Resources/"

# The "Spanly Microphone" audio driver, installed from the menu when first needed.
DRIVER="$APP/Contents/Resources/SpanlyMicrophone.driver"
mkdir -p "$DRIVER/Contents/MacOS"
cp "$ROOT/src/driver/Info.plist" "$DRIVER/Contents/"
clang++ -std=c++23 -bundle -O2 -Wall -Wextra -Wno-unused-parameter -arch arm64 -arch x86_64 -mmacosx-version-min=$MIN_MACOS \
    -framework CoreAudio -framework CoreFoundation "$ROOT"/src/driver/*.cpp -o "$DRIVER/Contents/MacOS/SpanlyMicrophone"

cat > "$APP/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleIdentifier</key><string>com.caigenix.spanly</string>
    <key>CFBundleName</key><string>Spanly</string>
    <key>CFBundleDisplayName</key><string>Spanly</string>
    <key>CFBundleExecutable</key><string>spanly</string>
    <key>CFBundleIconFile</key><string>AppIcon</string>
    <key>CFBundlePackageType</key><string>APPL</string>
    <key>CFBundleShortVersionString</key><string>$VERSION</string>
    <key>CFBundleVersion</key><string>$VERSION</string>
    <key>LSUIElement</key><true/>
    <key>LSMinimumSystemVersion</key><string>$MIN_MACOS</string>
    <key>NSHumanReadableCopyright</key><string>Spanly</string>
    <key>NSLocalNetworkUsageDescription</key>
    <string>Spanly lets your paired tablet connect over Wi-Fi when "Allow Wi-Fi Connection" is on.</string>
    <key>CFBundleURLTypes</key>
    <array><dict>
        <key>CFBundleURLName</key><string>com.caigenix.spanly</string>
        <key>CFBundleURLSchemes</key><array><string>spanly</string></array>
    </dict></array>
    <key>NSBonjourServices</key>
    <array><string>_spanly._tcp</string></array>
</dict>
</plist>
PLIST

echo "==> Signing"
identity=${SPANLY_SIGN_IDENTITY:-}
for name in "Spanly Local Signing" "SideScreen Local Signing"; do
    [[ -n "$identity" ]] && break
    identity=$(security find-identity -p codesigning | awk -v n="\"$name\"" 'index($0, n) {print $2; exit}')
done
if [[ -z "$identity" ]]; then
    echo "   ad-hoc (Screen Recording / Accessibility must be re-granted after each build)"
    identity=-
fi
runtime=()
[[ "$identity" == Developer\ ID* ]] && runtime=(--options runtime --timestamp)
# coreaudiod only loads audio plug-ins with a trusted signature: a Developer ID, or ad-hoc.
if [[ "$identity" == Developer\ ID* ]]; then
    codesign --force --sign "$identity" "${runtime[@]}" "$DRIVER"
else
    codesign --force --sign - "$DRIVER"
fi
codesign --force --sign "$identity" "${runtime[@]}" --identifier com.caigenix.spanly "$APP"
codesign --verify --strict "$APP"

(cd "$DIST" && rm -f Spanly-mac.zip && ditto -c -k --keepParent Spanly.app Spanly-mac.zip)
lipo -archs "$APP/Contents/MacOS/spanly"
ls -la "$APP/Contents/MacOS/spanly" | awk '{print "binary: " $5 " bytes"}'
echo "Built $APP (version $VERSION)"
