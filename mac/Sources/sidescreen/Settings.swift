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

    var quality: Quality {
        get { Quality(rawValue: d.string(forKey: "quality") ?? "") ?? .auto }
        set { d.set(newValue.rawValue, forKey: "quality") }
    }

    var position: Position {
        get { Position(rawValue: d.string(forKey: "position") ?? "") ?? .right }
        set { d.set(newValue.rawValue, forKey: "position") }
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
