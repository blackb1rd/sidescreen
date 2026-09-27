import Foundation

/// What the menu bar reads and changes. Main thread only.
extension Controller {
    /// Each streaming tablet's name and a short description of its stream.
    var streamStatus: [(tablet: String, detail: String)] {
        sessions.filter { $0.streaming && $0.size.w > 0 }.map { s in
            let codec = s.encoder?.codec == .hevc ? "HEVC" : "H.264"
            return (s.name, "\(s.size.w)×\(s.size.h) · \(codec) · \(s.onUsb ? "USB" : s.onWifi ? "Wi-Fi" : "adb (slower)")")
        }
    }

    var tablets: [UsbTablet] { usb?.tablets() ?? [] }

    var chosenSerial: String? { settings.deviceSerial ?? adb?.serial }

    func choose(_ t: UsbTablet) {
        settings.deviceSerial = t.serial
        settings.deviceName = t.name
        log("using \(t.name) (\(t.serial)) as the second screen")
    }

    func setWifi(_ enabled: Bool) {
        settings.wifi = enabled
        wifi.setEnabled(enabled)
    }

    /// New Wi-Fi secret: every paired tablet must be plugged in again before using Wi-Fi.
    func forgetPairedTablets() {
        settings.resetWifiSecret()
        wifi.setEnabled(false)
        wifi.setEnabled(settings.wifi)
        log("Wi-Fi pairing reset")
    }

    /// Restart capture with the current settings (e.g. sound on/off), keeping the displays.
    func restartCapture() {
        sessions.forEach { $0.restartCapture() }
    }

    /// Re-apply menu settings to a running stream.
    func applySettings(recreateDisplay: Bool) {
        pointer.restoreCursor = opts.restoreCursor ?? settings.restoreCursor
        if !recreateDisplay {
            let displays = sessions.compactMap { $0.virtual?.displayID }
            for s in sessions { s.virtual?.place(position, others: displays) }
            return
        }
        sessions.forEach { $0.recreateDisplay() }
    }

    /// Remember the tablet we connected to, so it is used again without adb.
    func rememberTablet() {
        guard let c = usb?.connected, !c.serial.isEmpty else { return }
        // Plugged in = trusted: hand over the Wi-Fi secret so it can connect wirelessly later.
        var pair = settings.wifiSecret
        pair.append(Data((Host.current().localizedName ?? "Mac").utf8.prefix(200)))
        usb?.send(.pair, pair)
        if settings.deviceSerial != c.serial || settings.deviceName != c.name {
            settings.deviceSerial = c.serial
            settings.deviceName = c.name
        }
    }
}
