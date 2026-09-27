#!/bin/zsh
# Build a distributable Spanly.app into mac/dist/ (plus a zip for GitHub releases).
#
#   - universal binary (Apple silicon + Intel), macOS 14 or later
#   - libusb compiled from source for the same targets and bundled in Contents/Frameworks
#     (so the app does not depend on Homebrew)
#   - signed with, in order of preference:
#       $SPANLY_SIGN_IDENTITY        e.g. "Developer ID Application: Your Name (TEAMID)"
#       "Spanly Local Signing"       self-signed, keeps permissions across local rebuilds
#       ad-hoc                           permissions must be re-granted after each build
#
# A Developer ID identity also enables the hardened runtime, as notarization requires.
# Notarize afterwards with:
#   xcrun notarytool submit mac/dist/Spanly-mac.zip --keychain-profile <profile> --wait
#   xcrun stapler staple mac/dist/Spanly.app
set -euo pipefail

VERSION=${SPANLY_VERSION:-0.1.0}
LIBUSB_VERSION=1.0.30
MIN_MACOS=14.0
ROOT="${0:A:h:h}"
MAC="$ROOT/mac"
DIST="$MAC/dist"
APP="$DIST/Spanly.app"
PREFIX="$MAC/.build/libusb-$LIBUSB_VERSION-universal"
LIB="$PREFIX/lib/libusb-1.0.0.dylib"

if [[ ! -f "$LIB" ]]; then
    echo "==> Building libusb $LIBUSB_VERSION (universal, macOS $MIN_MACOS+)"
    work=$(mktemp -d)
    curl -fsSL "https://github.com/libusb/libusb/releases/download/v$LIBUSB_VERSION/libusb-$LIBUSB_VERSION.tar.bz2" \
        | tar -xj -C "$work"
    flags="-arch arm64 -arch x86_64 -mmacosx-version-min=$MIN_MACOS"
    (cd "$work/libusb-$LIBUSB_VERSION" &&
        ./configure --prefix="$PREFIX" --disable-static --quiet CFLAGS="$flags -O2" LDFLAGS="$flags" &&
        make -j"$(sysctl -n hw.ncpu)" install >/dev/null)
    rm -rf "$work"
    install_name_tool -id @rpath/libusb-1.0.0.dylib "$LIB"
fi

echo "==> Building Spanly (universal)"
build=(swift build -c release --package-path "$MAC" --arch arm64 --arch x86_64)
PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig" "${build[@]}"
BIN="$(PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig" "${build[@]}" --show-bin-path)/spanly"

echo "==> Assembling $APP"
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Frameworks" "$APP/Contents/Resources"
cp "$BIN" "$APP/Contents/MacOS/spanly"
cp "$LIB" "$APP/Contents/Frameworks/"
cp "$MAC/Resources/AppIcon.icns" "$APP/Contents/Resources/"

# The "Spanly Microphone" audio driver, installed from the menu when first needed.
DRIVER="$APP/Contents/Resources/SpanlyMicrophone.driver"
mkdir -p "$DRIVER/Contents/MacOS"
cp "$ROOT/src/driver/Info.plist" "$DRIVER/Contents/"
clang++ -std=c++23 -bundle -O2 -Wall -Wextra -Wno-unused-parameter -arch arm64 -arch x86_64 -mmacosx-version-min=$MIN_MACOS \
    -framework CoreAudio -framework CoreFoundation "$ROOT"/src/driver/*.cpp -o "$DRIVER/Contents/MacOS/SpanlyMicrophone"
# Load libusb from inside the bundle, whatever path the linker recorded.
linked=$(otool -L "$APP/Contents/MacOS/spanly" | awk '/libusb-1.0/ {print $1}')
install_name_tool -change "$linked" @rpath/libusb-1.0.0.dylib "$APP/Contents/MacOS/spanly"
install_name_tool -add_rpath @executable_path/../Frameworks "$APP/Contents/MacOS/spanly" 2>/dev/null || true

cat > "$APP/Contents/Info.plist" <<EOF
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
EOF

echo "==> Signing"
identity=${SPANLY_SIGN_IDENTITY:-}
if [[ -z "$identity" ]]; then
    identity=$(security find-identity -p codesigning | awk '/"Spanly Local Signing"/ {print $2; exit}')
fi
if [[ -z "$identity" ]]; then
    echo "   ad-hoc (Screen Recording / Accessibility must be re-granted after each build)"
    identity=-
fi
runtime=()
[[ "$identity" == Developer\ ID* ]] && runtime=(--options runtime --timestamp)
codesign --force --sign "$identity" "${runtime[@]}" "$APP/Contents/Frameworks/libusb-1.0.0.dylib"
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
echo "Built $APP and $DIST/Spanly-mac.zip (version $VERSION)"
