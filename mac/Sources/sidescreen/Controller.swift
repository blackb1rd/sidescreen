import AppKit
import CoreMedia
import IOKit.ps
import QuartzCore

// MARK: - Controller

final class Controller {
    private let opts: Options
    let settings = Settings()
    private let server: Server
    private var usb: UsbLink?
    private let adb: Adb?
    private let capture = Capture()
    private let pointer = Pointer()
    private var encoder: Encoder?
    private var displayID: CGDirectDisplayID = 0
    private var size = (w: 0, h: 0)
    private var tabletHEVC = false
    // Flow control: frames sent but not yet decoded on the tablet (the tablet ACKs each one).
    private let flowLock = NSLock()
    private var nextFrameId: UInt32 = 1
    private var lastAcked: UInt32 = 0
    private var lastAckTime = CACurrentMediaTime()
    private var sentAt: [UInt32: CFTimeInterval] = [:]
    private let maxInFlight: UInt32 = 3
    private var activeBitrate = 0.0
    private var decodeMax = (0, 0) // largest size the tablet can decode at 60 fps (0 = unknown)
    private var onBattery = false
    private let stats = Stats()
    private var timers: [DispatchSourceTimer] = []
    private var waitingForKey = false
    private var displayOn = true
    private var restartWork: DispatchWorkItem?
    private var virtual: VirtualScreen?
    private var teardownWork: DispatchWorkItem?
    private var lastHello: (w: Int, h: Int, dpi: Int, caps: UInt32)?

    init(_ opts: Options) throws {
        self.opts = opts
        server = try Server(port: opts.port)
        adb = opts.manageAdb ? Adb(port: opts.port) : nil
        if opts.manageAdb && adb == nil { log("adb not found; set ADB=/path/to/adb or run adb reverse yourself") }
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
        for l in [server, usb].compactMap({ $0 }) as [Link] {
            l.onClient = { [weak self] in self?.clientConnected() }
            l.onHello = { [weak self] w, h, dpi, caps, maxW, maxH in
                DispatchQueue.main.async {
                    guard let self else { return }
                    self.decodeMax = (maxW, maxH)
                    self.rememberTablet()
                    self.hello(w, h, dpi, caps)
                }
            }
            l.onDisconnect = { [weak self] in DispatchQueue.main.async { self?.scheduleTeardown() } }
            l.onAck = { [weak self] id in self?.acked(id) }
            l.onTouch = { [weak self] a, x, y in
                guard let self, trusted || AXIsProcessTrusted() else { return }
                DispatchQueue.main.async { self.pointer.touch(action: a, x: x, y: y) }
            }
            l.onScroll = { [weak self] kind, phase, last, x, y, dx, dy in
                guard let self, trusted || AXIsProcessTrusted() else { return }
                DispatchQueue.main.async { self.pointer.scroll(kind: kind, phase: phase, last: last, x: x, y: y, dx: dx, dy: dy) }
            }
            l.onZoom = { [weak self] dir, x, y in
                guard let self, trusted || AXIsProcessTrusted() else { return }
                DispatchQueue.main.async { self.pointer.zoom(direction: dir, x: x, y: y) }
            }
        }
        capture.onFrame = { [weak self] pb, pts in
            guard let self, self.link.isConnected else { return }
            // USB link backed up: skip this capture rather than encode it. Skipping before the
            // encoder keeps the reference chain intact, so no keyframe is needed to recover.
            if self.tooManyInFlight() {
                self.stats.drop()
                return
            }
            self.encoder?.encode(pb, pts: pts)
        }
        capture.onStop = { [weak self] in self?.scheduleRestart() }

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
            guard let self, self.opts.displayName != nil || self.virtual != nil else { return }
            self.scheduleRestart()
        }

        adb?.startWatching()
        onBattery = Controller.isOnBattery()
        every(5) { [weak self] in self?.checkPower() }
        every(0.1) { [weak self] in self?.updateCursorVisibility() }
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
        enc.onFrame = { [weak self] data, isKey, config, started in self?.send(data, isKey: isKey, config: config, started: started) }
        let sizeChanged = size != (w, h)
        encoder = enc
        displayID = id
        pointer.displayID = id
        size = (w, h)

        Task {
            do {
                try await capture.start(displayID: id, width: w, height: h, fps: fps)
                log("capturing display \(id) at \(w)x\(h) @ \(fps) fps\(onBattery ? " (on battery)" : ""), \(enc.codec), \(bitrateMbps) Mbit/s")
                if link.isConnected {
                    if sizeChanged { sendSize() }
                    enc.requestKeyframe()
                }
            } catch {
                let hint = CGPreflightScreenCaptureAccess() ? "" : " — allow SideScreen (or the terminal running it) in Screen Recording settings"
                log("capture failed: \(error.localizedDescription)\(hint)")
                scheduleRestart(after: 3)
            }
        }
    }

    private func scheduleRestart(after delay: Double = 1) {
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
    private var link: Link {
        if let usb, usb.isConnected { return usb }
        return server
    }

    /// Show the cursor on the tablet only when the Mac's own mouse/trackpad has put it there,
    /// never because a touch borrowed it (that would leave a stray pointer in the picture).
    private func updateCursorVisibility() {
        guard encoder != nil, let here = CGEvent(source: nil)?.location else { return }
        capture.setShowsCursor(CGDisplayBounds(displayID).contains(here) && !pointer.touchActive)
    }

    private var onUsb: Bool { link is UsbLink }

    private var wantHiDPI: Bool {
        if let forced = opts.hiDPI { return forced }
        switch settings.quality {
        case .retina: return true
        case .standard: return false
        case .auto: return onUsb // Retina needs the bandwidth of the raw USB link
        }
    }

    private var position: String { opts.position ?? settings.position.rawValue }

    private var bitrateMbps: Double { opts.bitrateMbps ?? (onUsb ? 20 : 6) }

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

    private func hello(_ w: Int, _ h: Int, _ dpi: Int, _ caps: UInt32) {
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
        let (tw, th) = (max(w, h), min(w, h)) // landscape
        if let v = virtual, v.tabletW == tw, v.tabletH == th, v.hiDPI == wantHiDPI {
            // A repeated HELLO while the display is still being set up: the pipeline
            // start will send SIZE and a keyframe when it's ready.
            guard size.w > 0 else { return }
            if activeBitrate == bitrateMbps {
                startStream()
            } else {
                size = (0, 0) // re-send SIZE once the encoder restarts with the new bitrate
                scheduleRestart(after: 0.1)
            }
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
        log("created virtual display \(v.displayID) for a \(tw)x\(th) tablet (\(wantHiDPI ? "HiDPI" : "standard"), over \(onUsb ? "USB accessory" : "adb"))")
        // macOS needs a moment to publish the new display's modes and geometry.
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) { [weak self] in
            guard let self, self.virtual === v else { return }
            v.selectHiDPIMode()
            v.place(self.position)
            self.scheduleRestart(after: 0.5)
        }
    }

    /// Keep the display briefly across reconnects so windows don't jump around.
    private func scheduleTeardown() {
        guard opts.displayName == nil, virtual != nil else { return }
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
                    log("tablet gone; removed virtual display")
                }
            }
        }
        teardownWork = work
        DispatchQueue.main.asyncAfter(deadline: .now() + opts.lingerSeconds, execute: work)
    }

    private func startStream() {
        flowLock.withLock {
            lastAcked = nextFrameId &- 1
            lastAckTime = CACurrentMediaTime()
            sentAt.removeAll()
        }
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
        let id = flowLock.withLock { () -> UInt32 in
            let id = nextFrameId
            nextFrameId &+= 1
            sentAt[id] = started
            return id
        }
        var p = Data(capacity: data.count + 5)
        p.append(isKey ? 1 : 0)
        p.appendU32(id)
        p.append(data)
        link.send(.frame, p)
    }

    /// The tablet decoded frame `id` (and therefore everything before it).
    private func acked(_ id: UInt32) {
        let started: CFTimeInterval? = flowLock.withLock {
            guard id > lastAcked else { return nil }
            lastAcked = id
            lastAckTime = CACurrentMediaTime()
            let t = sentAt[id]
            sentAt = sentAt.filter { $0.key > id }
            return t
        }
        if let started { stats.latency((CACurrentMediaTime() - started) * 1000) }
    }

    /// Skip capturing when the tablet is behind. If ACKs stop (decoder dropped a frame,
    /// reconnect), give up waiting after 250 ms so the stream can't stall.
    private func tooManyInFlight() -> Bool {
        flowLock.withLock {
            let inFlight = nextFrameId &- 1 &- lastAcked
            if inFlight < maxInFlight { return false }
            if CACurrentMediaTime() - lastAckTime > 0.25 {
                lastAcked = nextFrameId &- 1
                lastAckTime = CACurrentMediaTime()
                sentAt.removeAll()
                return false
            }
            return true
        }
    }

    // MARK: Menu bar API (main thread)

    /// The tablet's name and a short description of the stream, or nil when not streaming.
    var streamStatus: (tablet: String, detail: String)? {
        guard link.isConnected, encoder != nil, size.w > 0 else { return nil }
        let name = usb?.connected?.name ?? settings.deviceName ?? "tablet"
        let codec = encoder?.codec == .hevc ? "HEVC" : "H.264"
        return (name, "\(size.w)×\(size.h) · \(codec) · \(onUsb ? "USB" : "adb (slower)")")
    }

    var tablets: [UsbTablet] { usb?.tablets() ?? [] }

    var chosenSerial: String? { settings.deviceSerial ?? adb?.serial }

    func choose(_ t: UsbTablet) {
        settings.deviceSerial = t.serial
        settings.deviceName = t.name
        log("using \(t.name) (\(t.serial)) as the second screen")
    }

    /// Re-apply menu settings to a running stream.
    func applySettings(recreateDisplay: Bool) {
        pointer.restoreCursor = opts.restoreCursor ?? settings.restoreCursor
        guard let v = virtual else { return }
        if !recreateDisplay {
            v.place(position)
            return
        }
        guard let hello = lastHello else { return }
        restartWork?.cancel()
        Task {
            await capture.stop()
            await MainActor.run {
                self.encoder = nil
                self.virtual = nil
                self.size = (0, 0)
            }
            // Give macOS a moment to remove the old display before making the new one.
            try? await Task.sleep(for: .milliseconds(500))
            await MainActor.run { self.hello(hello.w, hello.h, hello.dpi, hello.caps) }
        }
    }

    /// Remember the tablet we connected to, so it is used again without adb.
    private func rememberTablet() {
        guard let c = usb?.connected, !c.serial.isEmpty else { return }
        if settings.deviceSerial != c.serial || settings.deviceName != c.name {
            settings.deviceSerial = c.serial
            settings.deviceName = c.name
        }
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
