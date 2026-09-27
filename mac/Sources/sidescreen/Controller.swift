import AppKit
import CoreMedia
import IOKit.ps
import QuartzCore

// MARK: - Controller

final class Controller {
    let opts: Options
    let settings = Settings()
    private let server: Server
    var usb: UsbLink?
    var wifi: WifiLink!
    var tabletWindow: TabletWindow?
    var micPlayer: PCMPlayer?
    var tabletViewing = true // read on the capture queue; a stale read just encodes one extra frame
    private var currentBitrate = 0.0 // adaptive: starts at bitrateMbps, lowered when the link struggles
    let adb: Adb?
    let capture = Capture()
    let pointer = Pointer()
    var encoder: Encoder?
    private var displayID: CGDirectDisplayID = 0
    var size = (w: 0, h: 0)
    private var tabletHEVC = false
    private let flow = FlowControl()
    private var activeBitrate = 0.0
    private var decodeMax = (0, 0) // largest size the tablet can decode at 60 fps (0 = unknown)
    private var onBattery = false
    private let stats = Stats()
    private var timers: [DispatchSourceTimer] = []
    private var waitingForKey = false
    private var displayOn = true
    var restartWork: DispatchWorkItem?
    var virtual: VirtualScreen?
    private var teardownWork: DispatchWorkItem?
    var lastHello: (w: Int, h: Int, dpi: Int, caps: UInt32)?

    init(_ opts: Options) throws {
        self.opts = opts
        server = try Server(port: opts.port)
        adb = opts.manageAdb ? Adb(port: opts.port) : nil
        if opts.manageAdb && adb == nil { log("adb not found; set ADB=/path/to/adb or run adb reverse yourself") }
        let settings = self.settings
        wifi = WifiLink(secret: { settings.wifiSecret })
        if opts.usb {
            // The tablet chosen in the menu; with nothing chosen yet, the one adb sees (if any).
            usb = UsbLink(serial: { [weak self] in self?.settings.deviceSerial ?? self?.adb?.serial })
            if usb == nil { log("libusb unavailable; using adb only") }
        }
    }

    func start() {
        let trusted = AXIsProcessTrusted()
        if !trusted {
            log("touch input disabled until SideScreen (or the terminal running it) has Accessibility permission (System Settings > Privacy & Security > Accessibility)")
        }

        pointer.restoreCursor = opts.restoreCursor ?? settings.restoreCursor
        for l in [server, usb, wifi].compactMap({ $0 }) as [Link] {
            l.onClient = { [weak self] in self?.clientConnected() }
            l.onHello = { [weak self] w, h, dpi, caps, maxW, maxH in
                DispatchQueue.main.async {
                    guard let self else { return }
                    self.decodeMax = (maxW, maxH)
                    self.rememberTablet()
                    self.hello(w, h, dpi, caps)
                }
            }
            l.onDisconnect = { [weak self] in
                DispatchQueue.main.async {
                    self?.hideTabletScreen()
                    self?.tabletViewing = true
                    if self?.link.isConnected != true {
                        MacSpeakers.restore()
                        self?.updateMicrophone()
                    }
                    self?.scheduleTeardown()
                }
            }
            l.onAck = { [weak self] id in
                if let ms = self?.flow.acked(id) { self?.stats.latency(ms) }
            }
            l.onTouch = { [weak self] a, x, y in
                guard let self, trusted || AXIsProcessTrusted() else { return }
                DispatchQueue.main.async { self.pointer.touch(action: a, x: x, y: y) }
            }
            l.onScroll = { [weak self] kind, phase, last, x, y, dx, dy in
                guard let self, trusted || AXIsProcessTrusted() else { return }
                DispatchQueue.main.async { self.pointer.scroll(kind: kind, phase: phase, last: last, x: x, y: y, dx: dx, dy: dy) }
            }
            l.onPen = { [weak self] action, buttons, x, y, pressure in
                guard let self, trusted || AXIsProcessTrusted() else { return }
                DispatchQueue.main.async { self.pointer.pen(action: action, buttons: buttons, x: x, y: y, pressure: pressure) }
            }
            l.onZoom = { [weak self] dir, x, y in
                guard let self, trusted || AXIsProcessTrusted() else { return }
                DispatchQueue.main.async { self.pointer.zoom(direction: dir, x: x, y: y) }
            }
            wireTabletShare(l)
        }
        capture.onFrame = { [weak self] pb, pts in
            // Nothing to encode while the tablet's app is in the background (e.g. its own
            // screen is being shown on the Mac).
            guard let self, self.link.isConnected, self.tabletViewing else { return }
            // USB link backed up: skip this capture rather than encode it. Skipping before the
            // encoder keeps the reference chain intact, so no keyframe is needed to recover.
            if self.flow.tooManyInFlight() {
                self.stats.drop()
                return
            }
            self.encoder?.encode(pb, pts: pts)
        }
        capture.onStop = { [weak self] in self?.scheduleRestart() }
        capture.onAudio = { [weak self] pcm in
            guard let self, self.link.isConnected else { return }
            if self.opts.stats { self.stats.audio(pcm) }
            self.link.send(.audio, pcm)
        }

        let ws = NSWorkspace.shared.notificationCenter
        for n in [NSWorkspace.screensDidSleepNotification, NSWorkspace.willSleepNotification] {
            ws.addObserver(forName: n, object: nil, queue: .main) { [weak self] _ in self?.setDisplay(on: false) }
        }
        for n in [NSWorkspace.screensDidWakeNotification, NSWorkspace.didWakeNotification] {
            ws.addObserver(forName: n, object: nil, queue: .main) { [weak self] _ in self?.setDisplay(on: true) }
        }
        NotificationCenter.default.addObserver(
            forName: NSApplication.didChangeScreenParametersNotification, object: nil, queue: .main
        ) { [weak self] _ in
            guard let self, self.opts.displayName != nil || self.virtual != nil || (self.mirroring && self.encoder != nil) else { return }
            self.scheduleRestart()
        }

        adb?.startWatching()
        onBattery = Controller.isOnBattery()
        every(5) { [weak self] in self?.checkPower() }
        every(0.1) { [weak self] in self?.updateCursorVisibility() }
        every(1) { [weak self] in self?.adaptBitrate() }
        wifi.setEnabled(settings.wifi)
        if opts.stats {
            every(5) { [weak self] in
                if let line = self?.stats.report(seconds: 5) { log(line) }
            }
        }
        if let name = opts.displayName {
            startPipeline()
            log("listening on 127.0.0.1:\(opts.port), streaming display \"\(name)\"")
        } else {
            log("listening on 127.0.0.1:\(opts.port); the virtual display appears when the tablet connects")
        }
    }

    private func findDisplay() -> (CGDirectDisplayID, Int, Int)? {
        if mirroring {
            let id = CGMainDisplayID()
            guard let mode = CGDisplayCopyDisplayMode(id) else { return nil }
            return (id, mode.pixelWidth, mode.pixelHeight)
        }
        guard let name = opts.displayName else {
            guard let v = virtual, let mode = CGDisplayCopyDisplayMode(v.displayID) else { return nil }
            return (v.displayID, mode.pixelWidth, mode.pixelHeight)
        }
        for screen in NSScreen.screens where screen.localizedName.localizedCaseInsensitiveContains(name) {
            guard let num = screen.deviceDescription[NSDeviceDescriptionKey("NSScreenNumber")] as? NSNumber,
                  let mode = CGDisplayCopyDisplayMode(num.uint32Value) else { continue }
            return (num.uint32Value, mode.pixelWidth, mode.pixelHeight)
        }
        return nil
    }

    private func startPipeline() {
        guard displayOn else { return } // a sleeping display can't be captured; setDisplay(on:) restarts us
        guard let (id, pw, ph) = findDisplay() else {
            if let name = opts.displayName {
                log("display \"\(name)\" not found; retrying")
                scheduleRestart(after: 3)
            }
            return
        }
        var w = pw, h = ph
        if opts.maxWidth > 0 && w > opts.maxWidth {
            h = h * opts.maxWidth / w
            w = opts.maxWidth
        }
        // Fit within what the tablet's hardware decoder can handle (e.g. 2560x1440 on MediaTek).
        if decodeMax.0 > 0, decodeMax.1 > 0, w > decodeMax.0 || h > decodeMax.1 {
            let scale = min(Double(decodeMax.0) / Double(w), Double(decodeMax.1) / Double(h))
            w = Int(Double(w) * scale)
            h = Int(Double(h) * scale)
        }
        w &= ~15 // whole macroblocks
        h &= ~15

        let fps = currentFps
        guard let enc = Encoder(width: w, height: h, fps: fps, bitrate: Int(bitrateMbps * 1_000_000), codec: wantedCodec)
            ?? Encoder(width: w, height: h, fps: fps, bitrate: Int(bitrateMbps * 1_000_000), codec: .h264)
        else { return }
        if opts.stats { enc.stats = stats }
        activeBitrate = bitrateMbps
        currentBitrate = bitrateMbps
        enc.onFrame = { [weak self] data, isKey, config, started in self?.send(data, isKey: isKey, config: config, started: started) }
        let sizeChanged = size != (w, h)
        encoder = enc
        displayID = id
        pointer.displayID = id
        size = (w, h)

        Task {
            do {
                try await capture.start(displayID: id, width: w, height: h, fps: fps, audio: settings.sound != .mac)
                log("capturing display \(id) at \(w)x\(h) @ \(fps) fps\(onBattery ? " (on battery)" : ""), \(enc.codec), \(bitrateMbps) Mbit/s")
                if link.isConnected {
                    if sizeChanged { sendSize() }
                    enc.requestKeyframe()
                }
                await MainActor.run { self.updateSpeakers(); self.updateMicrophone() }
            } catch {
                let hint = CGPreflightScreenCaptureAccess() ? "" : " — allow SideScreen (or the terminal running it) in Screen Recording settings"
                log("capture failed: \(error.localizedDescription)\(hint)")
                scheduleRestart(after: 3)
            }
        }
    }

    func scheduleRestart(after delay: Double = 1) {
        DispatchQueue.main.async { [weak self] in
            guard let self else { return }
            self.restartWork?.cancel()
            let work = DispatchWorkItem { [weak self] in
                guard let self else { return }
                Task {
                    await self.capture.stop()
                    await MainActor.run { self.startPipeline() }
                }
            }
            self.restartWork = work
            DispatchQueue.main.asyncAfter(deadline: .now() + delay, execute: work)
        }
    }

    /// The raw USB link when the tablet is on it, otherwise TCP over adb.
    /// Where the tablet is: raw USB is best, then Wi-Fi, then TCP through adb.
    var link: Link {
        if let usb, usb.isConnected { return usb }
        if wifi.isConnected { return wifi }
        return server
    }

    var onWifi: Bool { link === wifi }

    /// Show the cursor on the tablet only when the Mac's own mouse/trackpad has put it there,
    /// never because a touch borrowed it (that would leave a stray pointer in the picture).
    private func updateCursorVisibility() {
        guard encoder != nil, let here = CGEvent(source: nil)?.location else { return }
        capture.setShowsCursor(CGDisplayBounds(displayID).contains(here) && !pointer.touchActive)
    }

    var onUsb: Bool { link is UsbLink }

    private var wantHiDPI: Bool {
        if let forced = opts.hiDPI { return forced }
        switch settings.quality {
        case .retina: return true
        case .standard: return false
        case .auto:
            // Keep an existing display when the link changes (e.g. the cable is unplugged and the
            // tablet carries on over Wi-Fi): rebuilding it would scatter its windows.
            if let v = virtual { return v.hiDPI }
            return onUsb // Retina needs the bandwidth of the raw USB link
        }
    }

    var position: String { opts.position ?? settings.position.rawValue }

    /// Mirror mode shows the Mac's main screen instead of a separate virtual display.
    private var mirroring: Bool { opts.displayName == nil && settings.mode == .mirror }

    /// Starting (and highest) bitrate for the current link; adaptive bitrate may go lower.
    private var bitrateMbps: Double { opts.bitrateMbps ?? (onUsb ? 20 : onWifi ? 12 : 6) }

    /// Every second: lower the bitrate when frames queue up or latency climbs, and creep back
    /// up when the link has headroom. Matters most on Wi-Fi.
    private func adaptBitrate() {
        guard let enc = encoder, link.isConnected else { return }
        let w = flow.takeWindow()
        guard w.acks + w.skipped >= 10 else { return } // screen idle: nothing to learn
        var target = currentBitrate
        if w.skipped > (w.acks + w.skipped) / 5 || w.avgLatencyMs > 90 {
            target = max(3, currentBitrate * 0.75)
        } else if w.skipped == 0 && w.avgLatencyMs < 60 {
            target = min(bitrateMbps, currentBitrate * 1.1)
        }
        guard abs(target - currentBitrate) >= 0.5 else { return }
        currentBitrate = target
        enc.setBitrate(Int(target * 1_000_000))
        if opts.stats { log(String(format: "bitrate -> %.1f Mbit/s (latency %.0f ms, %d skipped)", target, w.avgLatencyMs, w.skipped)) }
    }

    private var currentFps: Int { onBattery && opts.batteryFps > 0 ? min(opts.batteryFps, opts.fps) : opts.fps }

    private var wantedCodec: Codec { opts.codec == "hevc" && tabletHEVC ? .hevc : .h264 }

    private static func isOnBattery() -> Bool {
        guard let type = IOPSGetProvidingPowerSourceType(nil)?.takeRetainedValue() else { return false }
        return (type as String) == kIOPSBatteryPowerValue
    }

    private func checkPower() {
        let now = Controller.isOnBattery()
        guard now != onBattery else { return }
        onBattery = now
        guard opts.batteryFps > 0, opts.batteryFps < opts.fps else { return }
        log(now ? "Mac on battery -> \(opts.batteryFps) fps" : "Mac on AC power -> \(opts.fps) fps")
        if encoder != nil { scheduleRestart(after: 0.2) }
    }

    private func every(_ seconds: Double, _ block: @escaping () -> Void) {
        let t = DispatchSource.makeTimerSource(queue: .main)
        t.schedule(deadline: .now() + seconds, repeating: seconds)
        t.setEventHandler(handler: block)
        t.resume()
        timers.append(t)
    }

    private func sendSize() {
        var p = Data()
        p.appendU32(UInt32(size.w))
        p.appendU32(UInt32(size.h))
        p.appendU32(encoder?.codec.rawValue ?? Codec.h264.rawValue)
        link.send(.size, p)
    }

    private func clientConnected() {
        // With a virtual display we wait for the tablet's HELLO to know what size to make it.
        if opts.displayName != nil { startStream() }
    }

    func hello(_ w: Int, _ h: Int, _ dpi: Int, _ caps: UInt32) {
        lastHello = (w, h, dpi, caps)
        teardownWork?.cancel()
        teardownWork = nil
        let hevc = caps & 1 != 0
        if hevc != tabletHEVC {
            tabletHEVC = hevc
            if encoder != nil && encoder?.codec != wantedCodec {
                size = (0, 0) // force a SIZE (with the new codec) once the pipeline restarts
                scheduleRestart(after: 0.2)
                return
            }
        }
        guard opts.displayName == nil else { return }
        if mirroring {
            virtual = nil
            if size.w > 0 && activeBitrate == bitrateMbps {
                startStream()
            } else {
                size = (0, 0)
                scheduleRestart(after: 0.1)
            }
            return
        }
        let (tw, th) = (w, h) // as the tablet is held: landscape or portrait
        if let v = virtual, v.hiDPI == wantHiDPI, (v.tabletW, v.tabletH) == (th, tw) {
            // The tablet turned: reshape the same display so its windows stay on it.
            if v.resize(tabletW: tw, tabletH: th) {
                log("tablet rotated: display is now \(tw)x\(th)")
                size = (0, 0)
                DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) { [weak self] in
                    guard let self, self.virtual === v else { return }
                    v.selectMode()
                    self.scheduleRestart(after: 0.3)
                }
                return
            }
        }
        if let v = virtual, v.tabletW == tw, v.tabletH == th, v.hiDPI == wantHiDPI {
            // A repeated HELLO while the display is still being set up: the pipeline
            // start will send SIZE and a keyframe when it's ready.
            guard size.w > 0 else { return }
            if activeBitrate != bitrateMbps {
                // New link (e.g. USB -> Wi-Fi): retune the running encoder instead of restarting.
                activeBitrate = bitrateMbps
                currentBitrate = bitrateMbps
                encoder?.setBitrate(Int(bitrateMbps * 1_000_000))
            }
            startStream()
            return
        }
        virtual = nil
        size = (0, 0)
        guard let v = VirtualScreen(tabletW: tw, tabletH: th, dpi: dpi, hiDPI: wantHiDPI) else {
            // Usually the previous display (same serial) hasn't finished going away yet.
            log("could not create virtual display; retrying")
            DispatchQueue.main.asyncAfter(deadline: .now() + 1) { [weak self] in
                guard let self, self.virtual == nil, self.link.isConnected else { return }
                self.hello(w, h, dpi, caps)
            }
            return
        }
        virtual = v
        log("created virtual display \(v.displayID) for a \(tw)x\(th) tablet (\(wantHiDPI ? "HiDPI" : "standard"), over \(onUsb ? "USB accessory" : onWifi ? "Wi-Fi" : "adb"))")
        // macOS needs a moment to publish the new display's modes and geometry.
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) { [weak self] in
            guard let self, self.virtual === v else { return }
            v.selectMode()
            v.place(self.position)
            self.scheduleRestart(after: 0.5)
        }
    }

    /// Keep the display briefly across reconnects so windows don't jump around.
    private func scheduleTeardown() {
        guard opts.displayName == nil, virtual != nil || encoder != nil else { return }
        teardownWork?.cancel()
        let work = DispatchWorkItem { [weak self] in
            guard let self, !self.link.isConnected else { return }
            self.restartWork?.cancel()
            Task {
                await self.capture.stop()
                await MainActor.run {
                    self.encoder = nil
                    self.virtual = nil
                    self.size = (0, 0)
                    log("tablet gone; stopped streaming")
                }
            }
        }
        teardownWork = work
        DispatchQueue.main.asyncAfter(deadline: .now() + opts.lingerSeconds, execute: work)
    }

    /// The tablet's app came back to the front (restart with a keyframe) or went away.
    func setTabletViewing(_ viewing: Bool) {
        guard viewing != tabletViewing else { return }
        tabletViewing = viewing
        if viewing && encoder != nil { startStream() }
    }

    /// "Tablet Only" sound mutes the Mac while it is streaming to a tablet.
    func updateSpeakers() {
        if settings.sound == .tablet && link.isConnected && encoder != nil {
            MacSpeakers.mute()
        } else {
            MacSpeakers.restore()
        }
    }

    private func startStream() {
        updateSpeakers()
        updateMicrophone()
        flow.reset()
        flow.maxInFlight = onWifi ? 6 : 3
        sendSize()
        link.send(.display, Data([displayOn ? 1 : 0]))
        waitingForKey = true
        encoder?.requestKeyframe()
        capture.resendLast()
    }

    /// Called on the encoder's output thread.
    private func send(_ data: Data, isKey: Bool, config: Data?, started: CFTimeInterval) {
        guard link.isConnected else { return }
        if !isKey && waitingForKey { return }
        waitingForKey = false
        if let config { link.send(.config, config) }
        let id = flow.register(started: started)
        var p = Data(capacity: data.count + 5)
        p.append(isKey ? 1 : 0)
        p.appendU32(id)
        p.append(data)
        link.send(.frame, p)
    }

    private func setDisplay(on: Bool) {
        guard on != displayOn else { return }
        displayOn = on
        log(on ? "Mac display awake -> waking tablet" : "Mac display asleep -> sleeping tablet")
        link.send(.display, Data([on ? 1 : 0]))
        if on { adb?.wakeTablet() } else { adb?.sleepTablet() }
        if on && encoder != nil { scheduleRestart(after: 0.5) }
    }
}
