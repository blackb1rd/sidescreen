// SideScreen: use an Android tablet as a second Mac display over USB.
//
// A virtual display (CGVirtualDisplay) sized to the tablet is captured (ScreenCaptureKit),
// encoded (VideoToolbox HEVC/H.264, low-latency) and sent over raw USB in Android Open
// Accessory mode (Usb.swift), or over TCP through `adb reverse` as a slower fallback.
// Touch gestures come back on the same link and become mouse/scroll events (Pointer.swift).
// The menu bar item (MenuBar.swift) picks the tablet and holds the settings.
//
// Wire format, both directions: [0x5A][type:u8][length:u32 BE][payload]. The marker byte
// lets a reader resynchronise if it joins mid-stream (e.g. stale bytes left in the USB
// pipe from a previous session).

import AppKit

setvbuf(stdout, nil, _IOLBF, 0)

// MARK: - main

let opts = Options.parse()
let app = NSApplication.shared
app.setActivationPolicy(.accessory) // menu bar only, no Dock icon
let controller: Controller
do {
    controller = try Controller(opts)
} catch {
    // The port is taken: almost always another SideScreen that is already running.
    log("could not listen on port \(opts.port): \(error)")
    exit(1)
}
controller.start()
let menuBar = MenuBar(controller: controller)
let urlHandler = URLHandler(controller: controller)
menuBar.onboardIfNeeded()
app.run()
