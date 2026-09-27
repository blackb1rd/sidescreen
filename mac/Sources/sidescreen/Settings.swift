// Settings chosen in the menu bar, persisted in UserDefaults. Command-line options
// (see Options) override them for that run without being saved.

import Foundation

final class Settings {
    enum Quality: String, CaseIterable {
        case auto, retina, standard

        var title: String {
            switch self {
            case .auto: return "Automatic"
            case .retina: return "Retina (sharpest)"
            case .standard: return "Standard (lightest)"
            }
        }
    }

    enum Mode: String, CaseIterable {
        case extend, mirror

        var title: String {
            switch self {
            case .extend: return "Extend (separate second screen)"
            case .mirror: return "Mirror (same as the Mac's screen)"
            }
        }
    }

    enum Position: String, CaseIterable {
        case right, left, above, below, keep

        var title: String {
            switch self {
            case .keep: return "Don't Move (arrange in System Settings)"
            default: return rawValue.capitalized
            }
        }
    }

    private let d = UserDefaults.standard

    var mode: Mode {
        get { Mode(rawValue: d.string(forKey: "mode") ?? "") ?? .extend }
        set { d.set(newValue.rawValue, forKey: "mode") }
    }

    var quality: Quality {
        get { Quality(rawValue: d.string(forKey: "quality") ?? "") ?? .auto }
        set { d.set(newValue.rawValue, forKey: "quality") }
    }

    var position: Position {
        get { Position(rawValue: d.string(forKey: "position") ?? "") ?? .right }
        set { d.set(newValue.rawValue, forKey: "position") }
    }

    /// Accept paired tablets over Wi-Fi.
    var wifi: Bool {
        get { d.bool(forKey: "wifi") }
        set { d.set(newValue, forKey: "wifi") }
    }

    /// Secret given to tablets over USB so they can connect over Wi-Fi. Replacing it
    /// ("Forget Paired Tablets") locks out every tablet until it is plugged in again.
    var wifiSecret: Data {
        if let s = d.data(forKey: "wifiSecret"), s.count == 32 { return s }
        return resetWifiSecret()
    }

    @discardableResult
    func resetWifiSecret() -> Data {
        let s = WifiCrypto.newSecret()
        d.set(s, forKey: "wifiSecret")
        return s
    }

    /// Send the Mac's sound to the tablet's speakers.
    var audio: Bool {
        get { d.bool(forKey: "audio") }
        set { d.set(newValue, forKey: "audio") }
    }

    var restoreCursor: Bool {
        get { d.object(forKey: "restoreCursor") as? Bool ?? true }
        set { d.set(newValue, forKey: "restoreCursor") }
    }

    /// USB serial number of the tablet to use; the app never touches other devices.
    var deviceSerial: String? {
        get { d.string(forKey: "deviceSerial") }
        set { d.set(newValue, forKey: "deviceSerial") }
    }

    var deviceName: String? {
        get { d.string(forKey: "deviceName") }
        set { d.set(newValue, forKey: "deviceName") }
    }

    /// Whether the first-launch setup (login item, permission guide) has run.
    var onboarded: Bool {
        get { d.bool(forKey: "onboarded") }
        set { d.set(newValue, forKey: "onboarded") }
    }
}
