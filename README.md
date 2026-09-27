<p align="center"><img src="assets/mac-icon-1024.png" width="160" alt=""></p>

# SideScreen for Mac

Use an Android tablet as a second display for your Mac over a USB cable, with touch.

This repository is the Mac app. The tablet needs the **SideScreen** Android app.

- **Real extended display**: a virtual display sized to the tablet, Retina by default.
- **Fast, wired**: hardware HEVC/H.264 encoding on the Mac and hardware decoding on the tablet,
  over raw USB (Android Open Accessory mode). About 57 fps and ~40 ms from capture to decoded frame
  at 2304×1440 on a Redmi Pad 2.
- **Touch like a tablet**: swipe to scroll (with momentum), tap to click, hold-and-move to drag,
  hold or two-finger tap to right-click, pinch to zoom.
- **Follows the Mac**: starts when you plug the tablet in, sleeps and wakes with the Mac's display.

## Requirements

- macOS 14 or later, Apple silicon or Intel.
- An Android 11+ tablet or phone with the SideScreen app.
- A USB cable that carries data.

## Install

Download `SideScreen-mac.zip` from [Releases](../../releases), unzip it and move **SideScreen** to
Applications. Or build and install it yourself:

```sh
scripts/install-mac.sh             # build, install to ~/Applications, start
scripts/install-mac.sh --uninstall
```

SideScreen lives in the menu bar (no Dock icon) and opens at login. On first launch it asks for:

- **Screen Recording**: to show the Mac's screen on the tablet.
- **Accessibility**: to turn touches into clicks and scrolls.

Then connect the tablet. The Mac asks whether to use it as a second screen, and the tablet asks
whether to open SideScreen for the USB accessory; tick **Always**.

## Using it

| Menu | |
|---|---|
| Tablet | which connected Android device to use (only that device is ever touched) |
| Resolution | Automatic, Retina (sharpest) or Standard (lightest) |
| Position | where the display sits next to the main screen, or leave it where you arrange it |
| Return Pointer to Mac After Touch | put the pointer back on the Mac screen after each touch |
| Open at Login, Show Log, Quit | |

The log is at `~/Library/Logs/SideScreen.log`.

Tip: in System Settings → Desktop & Dock, set *Click wallpaper to reveal desktop* to *Only in
Stage Manager*, or a tap on an empty part of the tablet hides all your windows.

Command-line options (run `SideScreen.app/Contents/MacOS/sidescreen --help`) override the menu
settings for one run, for example `--stats` to log frame rate and latency.

## How it works

```
CGVirtualDisplay → ScreenCaptureKit → VideoToolbox ─┐        ┌─ MediaCodec → screen
                                                   USB (Android Open Accessory)
Pointer (CGEvent) ◀──── touch / scroll / zoom ──────┘        └─ gesture recognition
```

| File | |
|---|---|
| `Controller.swift` | ties it together: tablet sessions, flow control, settings |
| `VirtualScreen.swift` | the virtual display (private `CGVirtualDisplay` API) |
| `Capture.swift`, `Encoder.swift` | ScreenCaptureKit capture, VideoToolbox low-latency encoding |
| `Usb.swift` | Android Open Accessory link over libusb |
| `Server.swift`, `Adb.swift` | TCP fallback through `adb reverse` |
| `Protocol.swift` | message framing ([PROTOCOL.md](PROTOCOL.md)) |
| `Pointer.swift` | gestures to mouse, scroll and keyboard events |
| `MenuBar.swift`, `Settings.swift`, `Permissions.swift` | the UI |

## Build

```sh
swift build --package-path mac      # needs: brew install libusb pkg-config
swift test --package-path mac
scripts/build-mac-app.sh            # universal SideScreen.app with libusb bundled -> mac/dist/
python3 scripts/make-icons.py       # regenerate the icon from assets/icon-source.jpeg
```

`build-mac-app.sh` signs with `$SIDESCREEN_SIGN_IDENTITY` (for example a Developer ID). Otherwise
it uses a local `SideScreen Local Signing` certificate if you made one, which keeps the permissions
across rebuilds, or else ad-hoc signing.

## Releases

Pushing a tag like `v0.2.0` builds the app on GitHub Actions and attaches `SideScreen-mac.zip` to a
GitHub release. With these repository secrets set, it is also signed with a Developer ID and
notarized, so it opens without Gatekeeper warnings:

| Secret | |
|---|---|
| `MACOS_CERTIFICATE_P12` | base64 of the Developer ID Application certificate (.p12) |
| `MACOS_CERTIFICATE_PASSWORD` | its password |
| `MACOS_SIGN_IDENTITY` | e.g. `Developer ID Application: Name (TEAMID)` |
| `NOTARY_KEY_P8`, `NOTARY_KEY_ID`, `NOTARY_ISSUER_ID` | App Store Connect API key for notarization |

SideScreen can't be on the Mac App Store: it uses the private `CGVirtualDisplay` API and needs
Accessibility access, which the App Store sandbox doesn't allow.

## License

[GPL-3.0](LICENSE). libusb is LGPL-2.1 and is bundled as a separate dynamic library.
