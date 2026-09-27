import AppKit
import IOKit.ps

// MARK: - Controller

/// Owns the listeners (raw USB, Wi-Fi, adb) and one [TabletSession] per connected tablet.
/// Connections are matched to tablets by the ID in their HELLO, so a tablet moving from USB to
/// Wi-Fi keeps its display, and two tablets (e.g. one on USB, one on Wi-Fi) get one each.
/// Main thread only, except where noted.
final class Controller {
    /// Tablets whose app sends no ID (older versions) share this one.
    static let anonymousTablet = "anonymous"

    let opts: Options
    let settings = Settings()
    private let server: Server
    var usb: UsbLink?
    var wifi: WifiListener!
    let adb: Adb?
    let pointer = Pointer()
    private(set) var sessions: [TabletSession] = []
    var tabletWindow: TabletWindow?
    weak var shareSession: TabletSession?
    var micPlayer: PCMPlayer?
    weak var micSession: TabletSession?
    var onBattery = false
    var displayOn = true
    private var trusted = false
    private var timers: [DispatchSourceTimer] = []

    init(_ opts: Options) throws {
        self.opts = opts
        server = try Server(port: opts.port)
        adb = opts.manageAdb ? Adb(port: opts.port) : nil
        if opts.manageAdb && adb == nil { log("adb not found; set ADB=/path/to/adb or run adb reverse yourself") }
        let settings = self.settings
        wifi = WifiListener(secret: { settings.wifiSecret })
        if opts.usb {
            // The tablet chosen in the menu; with nothing chosen yet, the one adb sees (if any).
            usb = UsbLink(serial: { [weak self] in self?.settings.deviceSerial ?? self?.adb?.serial })
            if usb == nil { log("libusb unavailable; using adb only") }
        }
    }

    func start() {
        trusted = AXIsProcessTrusted()
        if !trusted {
            log("touch input disabled until SideScreen (or the terminal running it) has Accessibility permission (System Settings > Privacy & Security > Accessibility)")
        }
        pointer.restoreCursor = opts.restoreCursor ?? settings.restoreCursor
        for l in [server, usb].compactMap({ $0 }) as [Link] { wire(l) }
        wifi.onConnection = { [weak self] c in self?.wire(c) }

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
            guard let self else { return }
            for s in self.sessions where self.opts.displayName != nil || s.virtual != nil || (self.mirroring && s.encoder != nil) {
                s.scheduleRestart()
            }
        }

        adb?.startWatching()
        onBattery = Controller.isOnBattery()
        every(5) { [weak self] in self?.checkPower() }
        every(0.1) { [weak self] in self?.updateCursorVisibility() }
        every(1) { [weak self] in self?.sessions.forEach { $0.adaptBitrate() } }
        wifi.setEnabled(settings.wifi)
        if opts.stats {
            every(5) { [weak self] in
                for s in self?.sessions ?? [] {
                    if let line = s.stats.report(seconds: 5) { log("\(s.name): \(line)") }
                }
            }
        }
        log("listening on 127.0.0.1:\(opts.port); a virtual display appears when a tablet connects")
    }

    /// Route a connection's messages to the tablet it belongs to. Callbacks arrive on the
    /// link's own queue.
    private func wire(_ l: Link) {
        let trusted = self.trusted
        l.onHello = { [weak self, weak l] h in
            DispatchQueue.main.async {
                guard let self, let l else { return }
                self.hello(h, on: l)
            }
        }
        l.onDisconnect = { [weak self, weak l] in
            DispatchQueue.main.async {
                guard let self, let l else { return }
                self.closed(l)
            }
        }
        l.onAck = { [weak l] id in
            guard let s = l?.session, let ms = s.flow.acked(id) else { return }
            s.stats.latency(ms)
        }
        // Input goes to the display of the tablet it came from.
        let input = { [weak self, weak l] (event: @escaping (Pointer) -> Void) in
            guard let self, let s = l?.session, trusted || AXIsProcessTrusted() else { return }
            DispatchQueue.main.async {
                self.pointer.displayID = s.displayID
                event(self.pointer)
            }
        }
        l.onTouch = { a, x, y in input { $0.touch(action: a, x: x, y: y) } }
        l.onScroll = { kind, phase, last, x, y, dx, dy in
            input { $0.scroll(kind: kind, phase: phase, last: last, x: x, y: y, dx: dx, dy: dy) }
        }
        l.onPen = { action, buttons, x, y, pressure in
            input { $0.pen(action: action, buttons: buttons, x: x, y: y, pressure: pressure) }
        }
        l.onZoom = { dir, x, y in input { $0.zoom(direction: dir, x: x, y: y) } }
        l.onViewing = { [weak l] viewing in
            DispatchQueue.main.async { l?.session?.setViewing(viewing) }
        }
        wireTabletShare(l)
    }

    private func hello(_ h: Hello, on l: Link) {
        let id = h.id ?? Controller.anonymousTablet
        let s = sessions.first { $0.id == id } ?? {
            let s = TabletSession(id: id, controller: self)
            sessions.append(s)
            return s
        }()
        if l.session !== s {
            l.session?.remove(l)
            l.session = s
            s.add(l)
        }
        if l === usb { rememberTablet() }
        s.hello(h)
    }

    private func closed(_ l: Link) {
        guard let s = l.session else { return }
        s.remove(l) // a reconnect binds again with its HELLO
        l.session = nil
        guard !s.link.isConnected else { return } // still there on another link
        s.viewing = true
        if s === shareSession { hideTabletScreen() }
        updateSpeakers()
        updateMicrophone()
        s.scheduleTeardown()
    }

    func remove(_ s: TabletSession) {
        sessions.removeAll { $0 === s }
        // The next tablet now plays the Mac's sound.
        if settings.sound != .mac { primary?.restartCapture() }
        updateSpeakers()
        updateMicrophone()
    }

    /// The tablet the sound, microphone and "Show Tablet Screen" go to: the first one connected.
    var primary: TabletSession? { sessions.first { $0.link.isConnected } ?? sessions.first }

    var position: String { opts.position ?? settings.position.rawValue }

    /// Mirror mode shows the Mac's main screen instead of a separate virtual display.
    var mirroring: Bool { opts.displayName == nil && settings.mode == .mirror }

    private func updateCursorVisibility() {
        guard let here = CGEvent(source: nil)?.location else { return }
        for s in sessions { s.updateCursorVisibility(at: here, touchActive: pointer.touchActive) }
    }

    /// "Tablet Only" sound mutes the Mac while it is streaming to a tablet.
    func updateSpeakers() {
        if settings.sound == .tablet && sessions.contains(where: \.streaming) {
            MacSpeakers.mute()
        } else {
            MacSpeakers.restore()
        }
    }

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
        for s in sessions where s.encoder != nil { s.scheduleRestart(after: 0.2) }
    }

    private func every(_ seconds: Double, _ block: @escaping () -> Void) {
        let t = DispatchSource.makeTimerSource(queue: .main)
        t.schedule(deadline: .now() + seconds, repeating: seconds)
        t.setEventHandler(handler: block)
        t.resume()
        timers.append(t)
    }

    private func setDisplay(on: Bool) {
        guard on != displayOn else { return }
        displayOn = on
        log(on ? "Mac display awake -> waking tablets" : "Mac display asleep -> sleeping tablets")
        for s in sessions {
            s.link.send(.display, Data([on ? 1 : 0]))
            if on && s.encoder != nil { s.scheduleRestart(after: 0.5) }
        }
        if on { adb?.wakeTablet() } else { adb?.sleepTablet() }
    }
}
