<p align="center"><img src="assets/mac-icon-1024.png" width="160" alt=""></p>

# Spanly

Use an Android tablet or phone as a second display for your computer, over a USB cable or Wi-Fi,
with touch. One C++ app for **macOS**, **Windows** and **Linux**; the tablet needs the **Spanly**
Android app.

- **Real extended display**: a virtual display sized to the tablet (Retina on the Mac), or mirror
  the main screen. Turn the tablet and the display turns with it, keeping its windows.
- **Fast, wired**: hardware video encoding on the computer and hardware decoding on the tablet,
  over raw USB (Android Open Accessory mode), with the bitrate adapting to the connection.
- **Wi-Fi too**: plug in once to pair, then connect wirelessly (encrypted, AES-256-GCM). With the
  cable in, the tablet keeps Wi-Fi ready and carries on over it the moment you unplug. No router?
  Join the tablet's hotspot: Spanly finds it by itself.
- **Touch like a tablet**: swipe to scroll with momentum, tap to click, hold-and-move to drag, hold
  or two-finger tap to right-click, pinch to zoom, pen with pressure.
- **Two tablets at once**: one on USB and one on Wi-Fi (or both on Wi-Fi), each with its own display.
- **On the Mac, also**: the Mac's sound on the tablet (or the tablet only), the tablet's own screen
  in a Mac window controlled with mouse and keyboard, and the tablet's microphone as a Mac
  microphone.

| | macOS 14+ | Windows 10/11 (beta) | Linux (beta) |
|---|---|---|---|
| Extend (virtual display) | built in | [Virtual Display Driver](https://github.com/VirtualDrivers/Virtual-Display-Driver) | GNOME 46+ / KDE Plasma 6 |
| USB, Wi-Fi, adb | ✓ | ✓ (USB: WinUSB driver, see below) | ✓ |
| Menu | menu bar | tray icon | command line and settings file |
| Sound, microphone, tablet screen window | ✓ | not yet | not yet |

## Mac

Download `Spanly-mac.zip` from [Releases](../../releases), unzip it and move **Spanly** to
Applications, or build and install it yourself with `scripts/install-mac.sh`
(`--uninstall` removes it). Spanly lives in the menu bar and opens at login. On first launch it
asks for **Screen Recording** (to show your screen on the tablet) and **Accessibility** (to turn
touches into clicks). Then connect the tablet: the Mac asks whether to use it as a second screen,
and the tablet whether to open Spanly for the USB accessory (tick **Always**).

| Menu | |
|---|---|
| Show Tablet Screen on Computer | the tablet's own screen in a window (Esc = Back); controlling it needs **Spanly: control from Mac** on in the tablet's Accessibility settings |
| Tablet | which connected Android device to use (only that device is ever touched) |
| Mode, Resolution, Position | extend or mirror; Automatic, Retina or Standard; where the display sits |
| Allow Wi-Fi Connection | on by default; *Forget Paired Tablets* revokes every pairing |
| Sound | Computer Only, Computer and Tablet, or Tablet Only (mutes the computer while connected) |
| Use Tablet as Microphone | the tablet's mic as **Spanly Microphone** (*Install Spanly Microphone…* adds a small audio driver the first time) |
| Return Pointer After Touch, Open at Login, Show Log, Quit | |

The log is at `~/Library/Logs/Spanly.log`. `open spanly://show-tablet` and
`open spanly://hide-tablet` work from Shortcuts. Tip: in System Settings → Desktop & Dock, set
*Click wallpaper to reveal desktop* to *Only in Stage Manager*, or a tap on an empty part of the
tablet hides your windows; turn off *Displays have separate Spaces* to let a window span both
screens.

## Windows (beta)

1. **Extend** needs a virtual monitor: install the free, signed
   [Virtual Display Driver](https://github.com/VirtualDrivers/Virtual-Display-Driver). Spanly finds
   it and sets it to the tablet's resolution. **Mirror** needs nothing extra.
2. **Connecting**: Wi-Fi works once the tablet is paired (one wired session); adb works with the
   Android platform-tools and USB debugging on. For raw USB (fastest), after Spanly has switched
   the tablet once (it then appears as "Spanly"), use [Zadig](https://zadig.akeo.ie) to install
   **WinUSB** for that device's interface 0.
3. Run `spanly.exe`; the settings are in the tray icon's menu (and `%APPDATA%\Spanly\settings`).

## Linux (beta)

Needs the xdg-desktop-portal **RemoteDesktop** and **ScreenCast** portals (GNOME, KDE Plasma; Wayland
or X11). Extend mode needs virtual monitors (GNOME 46+, KDE Plasma 6); otherwise use Mirror.

```sh
sudo apt install gstreamer1.0-pipewire gstreamer1.0-plugins-good gstreamer1.0-plugins-bad \
  gstreamer1.0-vaapi                                   # plus gstreamer1.0-plugins-ugly for x264
sudo cp packaging/linux/70-spanly.rules /etc/udev/rules.d/ && sudo udevadm control --reload
./spanly
```

The desktop asks once to allow the screen and input; Spanly remembers the answer. Encoders are
tried in this order: VA-API (Intel, AMD), NVENC (NVIDIA), x264, OpenH264. Settings are in
`~/.config/spanly/settings`, the log in `~/.local/state/spanly/spanly.log`.

## Command line

Options override the settings for one run (`spanly --help`), for example `--stats` to log frame
rate, latency and bitrate, `--standard` / `--hidpi`, `--position left`, `--bitrate 20`,
`--no-usb`, `--no-adb`.

## How it works

```
virtual display → capture → hardware encoder ─┐          ┌─ MediaCodec → screen
                                             USB accessory / Wi-Fi / adb
mouse, scroll, keys ◀── touch / pen / zoom ───┘          └─ gesture recognition
```

| Folder | |
|---|---|
| `src/core` | the protocol ([PROTOCOL.md](PROTOCOL.md)), Wi-Fi crypto (mbedTLS), flow control, USB accessory (libusb), adb and Wi-Fi links, the beacon |
| `src/app` | one session per tablet (display, capture, encoder, adaptive bitrate), the controller matching connections to tablets, the menu, settings, adb |
| `src/platform/mac` | CGVirtualDisplay, ScreenCaptureKit, VideoToolbox, CGEvent, AppKit menu bar, CoreAudio: Apple's APIs from C++ through the Objective-C runtime |
| `src/platform/windows` | DXGI Desktop Duplication, Media Foundation H.264, SendInput, tray icon |
| `src/platform/linux` | xdg-desktop-portal (screen, virtual monitor, input) over D-Bus, GStreamer/PipeWire |
| `src/driver` | the Mac's "Spanly Microphone" audio driver (an AudioServerPlugIn loopback device) |

## Build

C++23 with CMake; mbedTLS and libusb are fetched and linked statically.

```sh
cmake -S . -B build -G Ninja && cmake --build build && ctest --test-dir build
scripts/build-mac.sh                  # universal Spanly.app -> dist/ (signed; see the script)
python3 scripts/make-icons.py         # regenerate the icon from assets/icon-source.jpeg
```

Linux needs `libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev libglib2.0-dev`. CI builds and
tests on all three systems, checks `clang-format` and `clang-tidy`, and runs the tests under
AddressSanitizer and UBSan.

Pushing a tag like `v0.2.0` attaches `Spanly-mac.zip` and the Windows and Linux builds to a GitHub
release. With these repository secrets, the Mac app is also signed with a Developer ID and
notarized: `MACOS_CERTIFICATE_P12` (base64 .p12), `MACOS_CERTIFICATE_PASSWORD`,
`MACOS_SIGN_IDENTITY`, `NOTARY_KEY_P8`, `NOTARY_KEY_ID`, `NOTARY_ISSUER_ID`.

## License

[GPL-3.0-or-later](LICENSE).
