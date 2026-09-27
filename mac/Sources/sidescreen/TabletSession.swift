import AppKit
import CoreMedia

// MARK: - TabletSession

/// One tablet: the connections it is using (the best connected one carries the stream: raw
/// USB, then Wi-Fi, then adb), its virtual display, and the capture -> encode -> send pipeline.
/// Main thread, except for the capture and encoder callbacks.
final class TabletSession {
    let id: String
    unowned let c: Controller
    let capture = Capture()
    var encoder: Encoder?
    var virtual: VirtualScreen?
    var size = (w: 0, h: 0)
    private(set) var displayID: CGDirectDisplayID = 0
    var viewing = true // read on the capture queue; a stale read just encodes one extra frame
    var lastHello: Hello?
    var restartWork: DispatchWorkItem?
    var teardownWork: DispatchWorkItem?
    var tabletHEVC = false
    var activeBitrate = 0.0
    var currentBitrate = 0.0 // adaptive: starts at bitrateMbps, lowered when the link struggles
    let flow = FlowControl()
    let stats = Stats()
    private var waitingForKey = false
    private let linksLock = NSLock()
    private var links: [Link] = []

    init(id: String, controller: Controller) {
        self.id = id
        c = controller
        capture.onFrame = { [weak self] pb, pts in
            // Nothing to encode while the tablet's app is in the background (e.g. its own
            // screen is being shown on the Mac).
            guard let self, self.viewing, self.link.isConnected else { return }
            // Link backed up: skip this capture rather than encode it. Skipping before the
            // encoder keeps the reference chain intact, so no keyframe is needed to recover.
            if self.flow.tooManyInFlight() {
                self.stats.drop()
                return
            }
            self.encoder?.encode(pb, pts: pts)
        }
        capture.onStop = { [weak self] in self?.scheduleRestart() }
        capture.onAudio = { [weak self] pcm in
            guard let self else { return }
            let l = self.link
            guard l.isConnected else { return }
            if self.c.opts.stats { self.stats.audio(pcm) }
            l.send(.audio, pcm)
        }
    }

    // MARK: Links

    func add(_ l: Link) {
        linksLock.withLock { if !links.contains(where: { $0 === l }) { links.append(l) } }
    }

    func remove(_ l: Link) {
        linksLock.withLock { links.removeAll { $0 === l } }
    }

    /// Where the tablet is now: raw USB is best, then Wi-Fi, then TCP through adb.
    var link: Link {
        let up = linksLock.withLock { links }.filter(\.isConnected)
        return up.first { $0 is UsbLink } ?? up.first { $0 is WifiConnection } ?? up.first ?? .none
    }

    var onUsb: Bool { link is UsbLink }
    var onWifi: Bool { link is WifiConnection }
    var streaming: Bool { link.isConnected && encoder != nil }

    var name: String {
        (link as? UsbLink)?.connected?.name ?? lastHello?.name ?? c.settings.deviceName ?? "tablet"
    }

    /// Serial number of the tablet's virtual display: macOS keeps each tablet's arrangement.
    var serial: UInt32 {
        guard id != Controller.anonymousTablet else { return 1 }
        let hash = id.utf8.reduce(UInt32(2_166_136_261)) { ($0 ^ UInt32($1)) &* 16_777_619 } // FNV-1a
        return max(2, hash & 0x7fff_ffff)
    }

    // MARK: Pipeline

    private func findDisplay() -> (CGDirectDisplayID, Int, Int)? {
        if c.mirroring {
            let id = CGMainDisplayID()
            guard let mode = CGDisplayCopyDisplayMode(id) else { return nil }
            return (id, mode.pixelWidth, mode.pixelHeight)
        }
        guard let name = c.opts.displayName else {
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

    func startPipeline() {
        guard c.displayOn else { return } // a sleeping display can't be captured; setDisplay(on:) restarts us
        guard let (id, pw, ph) = findDisplay() else {
            if let name = c.opts.displayName {
                log("display \"\(name)\" not found; retrying")
                scheduleRestart(after: 3)
            }
            return
        }
        var w = pw, h = ph
        let opts = c.opts
        if opts.maxWidth > 0 && w > opts.maxWidth {
            h = h * opts.maxWidth / w
            w = opts.maxWidth
        }
        // Fit within what the tablet's hardware decoder can handle (e.g. 2560x1440 on MediaTek).
        if let hello = lastHello, hello.maxW > 0, hello.maxH > 0, w > hello.maxW || h > hello.maxH {
            let scale = min(Double(hello.maxW) / Double(w), Double(hello.maxH) / Double(h))
            w = Int(Double(w) * scale)
            h = Int(Double(h) * scale)
        }
        w &= ~15 // whole macroblocks
        h &= ~15

        let fps = currentFps
        let bitrate = bitrateMbps
        guard let enc = Encoder(width: w, height: h, fps: fps, bitrate: Int(bitrate * 1_000_000), codec: wantedCodec)
            ?? Encoder(width: w, height: h, fps: fps, bitrate: Int(bitrate * 1_000_000), codec: .h264)
        else { return }
        if opts.stats { enc.stats = stats }
        activeBitrate = bitrate
        currentBitrate = bitrate
        enc.onFrame = { [weak self] data, isKey, config, started in self?.send(data, isKey: isKey, config: config, started: started) }
        let sizeChanged = size != (w, h)
        encoder = enc
        displayID = id
        size = (w, h)
        // Only the first tablet plays the Mac's sound: two would echo.
        let audio = c.settings.sound != .mac && c.primary === self

        Task {
            do {
                try await capture.start(displayID: id, width: w, height: h, fps: fps, audio: audio)
                log("capturing display \(id) at \(w)x\(h) @ \(fps) fps\(c.onBattery ? " (on battery)" : ""), \(enc.codec), \(bitrate) Mbit/s")
                await MainActor.run {
                    if self.link.isConnected {
                        self.flow.maxInFlight = self.onWifi ? 6 : 3
                        if sizeChanged { self.sendSize() }
                        enc.requestKeyframe()
                    }
                    self.c.updateSpeakers()
                    self.c.updateMicrophone()
                }
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

    /// Restart capture with the current settings (e.g. sound on/off), keeping the display.
    func restartCapture() {
        guard encoder != nil else { return }
        size = (0, 0) // re-send SIZE and a keyframe once capture restarts
        scheduleRestart(after: 0.1)
    }

    /// Stop streaming and remove the display.
    func stop() async {
        await MainActor.run {
            restartWork?.cancel()
            teardownWork?.cancel()
        }
        await capture.stop()
        await MainActor.run {
            encoder = nil
            virtual = nil
            size = (0, 0)
        }
    }

    /// Show the cursor on the tablet only when the Mac's own mouse/trackpad has put it there,
    /// never because a touch borrowed it (that would leave a stray pointer in the picture).
    func updateCursorVisibility(at here: CGPoint, touchActive: Bool) {
        guard encoder != nil else { return }
        capture.setShowsCursor(CGDisplayBounds(displayID).contains(here) && !touchActive)
    }

    var wantHiDPI: Bool {
        if let forced = c.opts.hiDPI { return forced }
        switch c.settings.quality {
        case .retina: return true
        case .standard: return false
        case .auto:
            // Keep an existing display when the link changes (e.g. the cable is unplugged and the
            // tablet carries on over Wi-Fi): rebuilding it would scatter its windows.
            if let v = virtual { return v.hiDPI }
            return onUsb // Retina needs the bandwidth of the raw USB link
        }
    }

    /// Starting (and highest) bitrate for the current link; adaptive bitrate may go lower.
    var bitrateMbps: Double { c.opts.bitrateMbps ?? (onUsb ? 20 : onWifi ? 12 : 6) }

    /// Every second: lower the bitrate when frames queue up or latency climbs, and creep back
    /// up when the link has headroom. Matters most on Wi-Fi.
    func adaptBitrate() {
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
        if c.opts.stats { log(String(format: "\(name): bitrate -> %.1f Mbit/s (latency %.0f ms, %d skipped)", target, w.avgLatencyMs, w.skipped)) }
    }

    private var currentFps: Int {
        let o = c.opts
        return c.onBattery && o.batteryFps > 0 ? min(o.batteryFps, o.fps) : o.fps
    }

    var wantedCodec: Codec { c.opts.codec == "hevc" && tabletHEVC ? .hevc : .h264 }

    private func sendSize() {
        var p = Data()
        p.appendU32(UInt32(size.w))
        p.appendU32(UInt32(size.h))
        p.appendU32(encoder?.codec.rawValue ?? Codec.h264.rawValue)
        link.send(.size, p)
    }

    /// The tablet's app came back to the front (restart with a keyframe) or went away.
    func setViewing(_ on: Bool) {
        guard on != viewing else { return }
        viewing = on
        if on && encoder != nil { startStream() }
    }

    func startStream() {
        c.updateSpeakers()
        c.updateMicrophone()
        flow.reset()
        flow.maxInFlight = onWifi ? 6 : 3
        sendSize()
        link.send(.display, Data([c.displayOn ? 1 : 0]))
        waitingForKey = true
        encoder?.requestKeyframe()
        capture.resendLast()
    }

    /// Called on the encoder's output thread.
    private func send(_ data: Data, isKey: Bool, config: Data?, started: CFTimeInterval) {
        let l = link
        guard l.isConnected else { return }
        if !isKey && waitingForKey { return }
        waitingForKey = false
        if let config { l.send(.config, config) }
        let id = flow.register(started: started)
        var p = Data(capacity: data.count + 5)
        p.append(isKey ? 1 : 0)
        p.appendU32(id)
        p.append(data)
        l.send(.frame, p)
    }
}
