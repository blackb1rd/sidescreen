import Foundation

// MARK: - adb

/// Keeps `adb reverse` alive across replugs, launches the app, and mirrors Mac sleep/wake.
final class Adb {
    static let appComponent = "dev.blackb1rd.spanly/.MainActivity"

    private let path: String
    private let port: UInt16
    private let queue = DispatchQueue(label: "adb")
    private var timer: DispatchSourceTimer?
    private var ready = false
    private let serialLock = NSLock()
    private var _serial: String?

    /// USB serial number of the connected tablet, so the USB link only ever claims that device.
    var serial: String? { serialLock.withLock { _serial } }

    init?(port: UInt16) {
        let candidates = [
            ProcessInfo.processInfo.environment["ADB"],
            NSHomeDirectory() + "/Library/Android/sdk/platform-tools/adb",
            "/opt/homebrew/bin/adb",
            "/usr/local/bin/adb",
        ].compactMap { $0 }
        guard let p = candidates.first(where: { FileManager.default.isExecutableFile(atPath: $0) }) else { return nil }
        path = p
        self.port = port
    }

    @discardableResult
    private func run(_ args: [String]) -> String {
        let proc = Process()
        proc.executableURL = URL(fileURLWithPath: path)
        proc.arguments = args
        let pipe = Pipe()
        proc.standardOutput = pipe
        proc.standardError = pipe
        do { try proc.run() } catch { return "" }
        let out = pipe.fileHandleForReading.readDataToEndOfFile()
        proc.waitUntilExit()
        return String(decoding: out, as: UTF8.self)
    }

    func startWatching() {
        let t = DispatchSource.makeTimerSource(queue: queue)
        t.schedule(deadline: .now(), repeating: 2)
        t.setEventHandler { [weak self] in self?.tick() }
        t.resume()
        timer = t
    }

    private func tick() {
        guard run(["get-state"]).trimmingCharacters(in: .whitespacesAndNewlines) == "device" else {
            if ready { log("tablet unplugged") }
            ready = false
            return
        }
        let sn = run(["get-serialno"]).trimmingCharacters(in: .whitespacesAndNewlines)
        if !sn.isEmpty { serialLock.withLock { _serial = sn } } // keep the last known serial
        if !run(["reverse", "--list"]).contains("tcp:\(port)") {
            run(["reverse", "tcp:\(port)", "tcp:\(port)"])
            log("adb reverse tcp:\(port) set up")
            ready = false
        }
        if !ready {
            ready = true
            launchApp()
        }
    }

    private func launchApp() {
        run(["shell", "am", "start", "-n", Adb.appComponent])
    }

    func sleepTablet() {
        queue.async { self.run(["shell", "input", "keyevent", "KEYCODE_SLEEP"]) }
    }

    func wakeTablet() {
        queue.async {
            self.run(["shell", "input", "keyevent", "KEYCODE_WAKEUP"])
            self.launchApp()
        }
    }
}
