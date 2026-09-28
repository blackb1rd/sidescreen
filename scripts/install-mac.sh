#!/bin/zsh
# Build Spanly.app, install it to ~/Applications and start it.
# The app registers itself to open at login (toggle in its menu bar menu).
#
# Usage: scripts/install-mac.sh             build + install + start
#        scripts/install-mac.sh --uninstall quit and remove the app (and its microphone driver)
set -euo pipefail

ROOT="${0:A:h:h}"
APP="$HOME/Applications/Spanly.app"

quit_running() {
    osascript -e 'tell application id "com.caigenix.spanly" to quit' 2>/dev/null || true
    pkill -f "Spanly.app/Contents/MacOS/spanly" 2>/dev/null || true
    sleep 1
}

if [[ "${1:-}" == "--uninstall" ]]; then
    quit_running
    rm -rf "$APP"
    if [[ -d /Library/Audio/Plug-Ins/HAL/SpanlyMicrophone.driver ]]; then
        echo "Removing the Spanly Microphone driver (needs your password)"
        sudo rm -rf /Library/Audio/Plug-Ins/HAL/SpanlyMicrophone.driver && sudo killall coreaudiod
    fi
    echo "Spanly removed. (Remove it from Login Items in System Settings if it is still listed.)"
    exit 0
fi

"$ROOT/scripts/build-mac.sh"
quit_running
mkdir -p "${APP:h}"
rm -rf "$APP"
cp -R "$ROOT/dist/Spanly.app" "$APP"
open "$APP"
echo "Installed and started $APP — look for its icon in the menu bar."
