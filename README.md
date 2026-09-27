# SideScreen for Windows and Linux (beta)

Use an Android tablet as a second display for your PC, over a USB cable or Wi-Fi, with touch.

> **Beta: needs testers.** The shared core (protocol, USB accessory link, encrypted Wi-Fi,
> flow control) has been tested end to end with a real tablet. The Windows and Linux screen-capture
> and input backends build and pass CI but haven't been run on real hardware yet. Please try it
> and [open an issue](../../issues) with your OS, desktop, GPU and what happened.

The tablet needs the **SideScreen** Android app. The Mac version is
[blackb1rd/sidescreen](https://github.com/blackb1rd/sidescreen); the wire protocol is described in
its [PROTOCOL.md](https://github.com/blackb1rd/sidescreen/blob/main/PROTOCOL.md).

## Features

- **Extend or mirror**: a separate second screen (a virtual monitor) or the same picture as your
  main screen.
- **Wired or wireless**: raw USB (Android Open Accessory), TCP through adb, or Wi-Fi. Plug the tablet
  in once to pair it, and from then on it connects over Wi-Fi, encrypted (AES-256-GCM).
- **Low latency**: H.264 with hardware encoders where available. The tablet acknowledges every frame,
  and the bitrate adapts to the connection.
- **Touch**: tap, drag, swipe to scroll with momentum, long press or two-finger tap for right
  click, pinch to zoom, and pen input.

## Linux

Works with desktops that support the xdg-desktop-portal **RemoteDesktop** and **ScreenCast**
portals: GNOME and KDE Plasma, on Wayland or X11. Extend mode needs a portal that can create
**virtual monitors** (GNOME 46+, KDE Plasma 6); otherwise use `--mirror`.

1. Install the runtime pieces (Debian/Ubuntu names):
   ```sh
   sudo apt install libusb-1.0-0 gstreamer1.0-pipewire gstreamer1.0-plugins-good \
     gstreamer1.0-plugins-bad gstreamer1.0-vaapi   # plus gstreamer1.0-plugins-ugly for x264
   ```
   Encoders are tried in this order: VA-API (Intel/AMD), NVENC (NVIDIA), x264, OpenH264.
2. Let your user reach the tablet over USB without root:
   ```sh
   sudo cp packaging/linux/70-sidescreen.rules /etc/udev/rules.d/
   sudo udevadm control --reload
   ```
3. Run `sidescreen` and plug in the tablet. The desktop asks once to allow remote interaction
   (screen + input); SideScreen remembers the answer.

## Windows

1. **Extend mode** needs a virtual monitor: install the free, signed
   [Virtual Display Driver](https://github.com/VirtualDrivers/Virtual-Display-Driver).
   SideScreen finds it and sets it to the tablet's resolution. **Mirror mode** (`--mirror`) needs
   nothing extra.
2. **Connecting**:
   - **Wi-Fi**: works without drivers once the tablet is paired. Pairing needs one wired session
     (adb or raw USB).
   - **adb**: install the Android platform-tools and turn on USB debugging on the tablet.
     SideScreen sets up `adb reverse` itself.
   - **Raw USB** (fastest): Windows has no driver for Android's accessory mode. After SideScreen
     has switched the tablet once (it then appears as "SideScreen"), use [Zadig](https://zadig.akeo.ie)
     to install **WinUSB** for that device's interface 0.
3. Run `sidescreen.exe`.

## Usage

```
sidescreen [options]

  --mirror             show the same picture as the main screen (default: extend)
  --wifi on|off        let tablets that were plugged in once connect over Wi-Fi (saved)
  --forget-pairing     new Wi-Fi secret: every tablet must be plugged in again
  --list-devices       list connected Android devices
  --device SERIAL      use this device as the tablet (saved)
  --fps N              frame rate (default 60)
  --bitrate MBPS       starting/maximum bitrate (default: 20 USB, 12 Wi-Fi, 6 adb)
  --no-adb             don't manage adb
  --no-usb             don't use USB accessory mode
  --stats              log frame rate, latency and bitrate
```

Settings live in `~/.config/sidescreen/config` (Linux) or `%APPDATA%\SideScreen\config` (Windows).
With exactly one Android device plugged in, SideScreen picks it automatically.

## How it works

| File | |
|---|---|
| `session.rs` | ties it together: picks the best link, stream geometry, frames, input, adaptive bitrate |
| `usb.rs` | Android Open Accessory over libusb (only the chosen device is ever switched) |
| `wifi.rs`, `crypto.rs` | Bonjour-advertised Wi-Fi link, HKDF-SHA256 + AES-256-GCM (same as Mac and Android) |
| `adb.rs`, `link.rs` | TCP through `adb reverse`; shared link plumbing |
| `protocol.rs`, `flow.rs` | message framing with resync; frames in flight, ACK latency, bitrate adaptation |
| `platform/linux/` | xdg-desktop-portal (screen, virtual monitor, input) + GStreamer/PipeWire encoding |
| `platform/windows/` | DXGI Desktop Duplication, Media Foundation H.264, SendInput |
| `platform/test_pattern.rs` | a synthetic source (`--features test-pattern`) to try the link on any OS |

## Build

```sh
# Linux: sudo apt install pkg-config libusb-1.0-0-dev libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev
cargo build --release
cargo test

# Any OS, including macOS: the link with a test pattern instead of the desktop
cargo run --features test-pattern -- --test-pattern --stats
```

Pushing a `v*` tag builds Linux and Windows binaries and attaches them to a GitHub release.

## License

[GPL-3.0-or-later](LICENSE).
