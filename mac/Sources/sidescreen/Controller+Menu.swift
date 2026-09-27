import Foundation

/// What the menu bar reads and changes. Main thread only.
extension Controller {
    /// The tablet's name and a short description of the stream, or nil when not streaming.
    var streamStatus: (tablet: String, detail: String)? {
        guard link.isConnected, encoder != nil, size.w > 0 else { return nil }
        let name = usb?.connected?.name ?? settings.deviceName ?? "tablet"
        let codec = encoder?.codec == .hevc ? "HEVC" : "H.264"
        return (name, "\(size.w)×\(size.h) · \(codec) · \(onUsb ? "USB" : onWifi ? "Wi-Fi" : "adb (slower)")")
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

    /// Restart capture with the current settings (e.g. sound on/off), keeping the display.
    func restartCapture() {
        guard encoder != nil else { return }
        size = (0, 0) // re-send SIZE and a keyframe once capture restarts
        scheduleRestart(after: 0.1)
    }

    /// Re-apply menu settings to a running stream.
    func applySettings(recreateDisplay: Bool) {
        pointer.restoreCursor = opts.restoreCursor ?? settings.restoreCursor
        if !recreateDisplay {
            virtual?.place(position)
            return
        }
        guard let hello = lastHello, link.isConnected else { return }
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
