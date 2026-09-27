#!/bin/zsh
# Build a distributable SideScreen.app into mac/dist/ (plus a zip for GitHub releases).
#
#   - universal binary (Apple silicon + Intel), macOS 14 or later
#   - libusb compiled from source for the same targets and bundled in Contents/Frameworks
#     (so the app does not depend on Homebrew)
#   - signed with, in order of preference:
#       $SIDESCREEN_SIGN_IDENTITY        e.g. "Developer ID Application: Your Name (TEAMID)"
#       "SideScreen Local Signing"       self-signed, keeps permissions across local rebuilds
#       ad-hoc                           permissions must be re-granted after each build
#
# A Developer ID identity also enables the hardened runtime, as notarization requires.
# Notarize afterwards with:
#   xcrun notarytool submit mac/dist/SideScreen-mac.zip --keychain-profile <profile> --wait
#   xcrun stapler staple mac/dist/SideScreen.app
set -euo pipefail

VERSION=${SIDESCREEN_VERSION:-0.1.0}
LIBUSB_VERSION=1.0.30
MIN_MACOS=14.0
ROOT="${0:A:h:h}"
MAC="$ROOT/mac"
DIST="$MAC/dist"
APP="$DIST/SideScreen.app"
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

echo "==> Building SideScreen (universal)"
build=(swift build -c release --package-path "$MAC" --arch arm64 --arch x86_64)
PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig" "${build[@]}"
BIN="$(PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig" "${build[@]}" --show-bin-path)/sidescreen"

echo "==> Assembling $APP"
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Frameworks" "$APP/Contents/Resources"
cp "$BIN" "$APP/Contents/MacOS/sidescreen"
cp "$LIB" "$APP/Contents/Frameworks/"
cp "$MAC/Resources/AppIcon.icns" "$APP/Contents/Resources/"
# Load libusb from inside the bundle, whatever path the linker recorded.
linked=$(otool -L "$APP/Contents/MacOS/sidescreen" | awk '/libusb-1.0/ {print $1}')
install_name_tool -change "$linked" @rpath/libusb-1.0.0.dylib "$APP/Contents/MacOS/sidescreen"
install_name_tool -add_rpath @executable_path/../Frameworks "$APP/Contents/MacOS/sidescreen" 2>/dev/null || true

cat > "$APP/Contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleIdentifier</key><string>dev.blackb1rd.sidescreen</string>
    <key>CFBundleName</key><string>SideScreen</string>
    <key>CFBundleDisplayName</key><string>SideScreen</string>
    <key>CFBundleExecutable</key><string>sidescreen</string>
    <key>CFBundleIconFile</key><string>AppIcon</string>
    <key>CFBundlePackageType</key><string>APPL</string>
    <key>CFBundleShortVersionString</key><string>$VERSION</string>
    <key>CFBundleVersion</key><string>$VERSION</string>
    <key>LSUIElement</key><true/>
    <key>LSMinimumSystemVersion</key><string>$MIN_MACOS</string>
    <key>NSHumanReadableCopyright</key><string>SideScreen</string>
    <key>NSLocalNetworkUsageDescription</key>
    <string>SideScreen lets your paired tablet connect over Wi-Fi when "Allow Wi-Fi Connection" is on.</string>
    <key>CFBundleURLTypes</key>
    <array><dict>
        <key>CFBundleURLName</key><string>dev.blackb1rd.sidescreen</string>
        <key>CFBundleURLSchemes</key><array><string>sidescreen</string></array>
    </dict></array>
    <key>NSBonjourServices</key>
    <array><string>_sidescreen._tcp</string></array>
</dict>
</plist>
EOF

echo "==> Signing"
identity=${SIDESCREEN_SIGN_IDENTITY:-}
if [[ -z "$identity" ]]; then
    identity=$(security find-identity -p codesigning | awk '/"SideScreen Local Signing"/ {print $2; exit}')
fi
if [[ -z "$identity" ]]; then
    echo "   ad-hoc (Screen Recording / Accessibility must be re-granted after each build)"
    identity=-
fi
runtime=()
[[ "$identity" == Developer\ ID* ]] && runtime=(--options runtime --timestamp)
codesign --force --sign "$identity" "${runtime[@]}" "$APP/Contents/Frameworks/libusb-1.0.0.dylib"
codesign --force --sign "$identity" "${runtime[@]}" --identifier dev.blackb1rd.sidescreen "$APP"
codesign --verify --strict "$APP"

(cd "$DIST" && rm -f SideScreen-mac.zip && ditto -c -k --keepParent SideScreen.app SideScreen-mac.zip)
lipo -archs "$APP/Contents/MacOS/sidescreen"
echo "Built $APP and $DIST/SideScreen-mac.zip (version $VERSION)"
