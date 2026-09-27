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

    init() {
        // Earlier names of this app (SideScreen, then Spanly under another ID): carry their settings
        // (e.g. the Wi-Fi pairing) over once.
        if !d.bool(forKey: "migratedFromOldName") {
            for name in ["dev.blackb1rd.spanly", "dev.blackb1rd." + "sidescreen"] {
                for (key, value) in d.persistentDomain(forName: name) ?? [:] where d.object(forKey: key) == nil {
                    d.set(value, forKey: key)
                }
            }
        }
        d.set(true, forKey: "migratedFromOldName")
    }

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

    /// Accept paired tablets over Wi-Fi (on unless turned off: only tablets plugged in once can
    /// connect, and it lets a tablet carry on at once when its cable comes out).
    var wifi: Bool {
        get { d.object(forKey: "wifi") as? Bool ?? true }
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

    enum Sound: String, CaseIterable {
        case mac, both, tablet

        var title: String {
            switch self {
            case .mac: return "Mac Only"
            case .both: return "Mac and Tablet"
            case .tablet: return "Tablet Only (mutes the Mac)"
            }
        }
    }

    /// Where the Mac's sound plays while a tablet is connected.
    var sound: Sound {
        get {
            if let s = Sound(rawValue: d.string(forKey: "sound") ?? "") { return s }
            return d.bool(forKey: "audio") ? .both : .mac // earlier on/off setting
        }
        set { d.set(newValue.rawValue, forKey: "sound") }
    }

    /// Use the tablet's microphone as "Spanly Microphone" (needs the audio driver).
    var tabletMicrophone: Bool {
        get { d.bool(forKey: "tabletMicrophone") }
        set { d.set(newValue, forKey: "tabletMicrophone") }
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
