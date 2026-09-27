// The menu bar item: status, which tablet to use, and settings.

import AppKit
import ServiceManagement

/// NSMenuItem that runs a closure.
private final class ActionItem: NSMenuItem {
    private let handler: () -> Void

    init(_ title: String, key: String = "", checked: Bool = false, enabled: Bool = true, _ handler: @escaping () -> Void) {
        self.handler = handler
        super.init(title: title, action: #selector(run), keyEquivalent: key)
        target = self
        state = checked ? .on : .off
        isEnabled = enabled
    }

    required init(coder: NSCoder) { fatalError("not used") }

    @objc private func run() { handler() }
}

final class MenuBar: NSObject, NSMenuDelegate {
    private let controller: Controller
    private let item = NSStatusBar.system.statusItem(withLength: NSStatusItem.squareLength)
    private let menu = NSMenu()
    private var offered = Set<String>() // tablets we already asked about this session
    private var timer: Timer?

    init(controller: Controller) {
        self.controller = controller
        super.init()
        menu.delegate = self
        menu.autoenablesItems = false
        item.menu = menu
        updateIcon()
        timer = Timer.scheduledTimer(withTimeInterval: 2, repeats: true) { [weak self] _ in
            self?.updateIcon()
            self?.offerTabletIfNeeded()
        }
    }

    // MARK: First launch

    func onboardIfNeeded() {
        let settings = controller.settings
        if !settings.onboarded {
            settings.onboarded = true
            try? SMAppService.mainApp.register() // open at login by default; toggle in the menu
        }
        if !Permissions.allGranted {
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) { Permissions.showGuide() }
        }
    }

    /// With no tablet chosen, offer the first Android device that shows up.
    private func offerTabletIfNeeded() {
        guard controller.chosenSerial == nil,
              let t = controller.tablets.first(where: { !offered.contains($0.serial) }) else { return }
        offered.insert(t.serial)
        NSApp.activate(ignoringOtherApps: true)
        let alert = NSAlert()
        alert.messageText = "Use “\(t.name)” as a second screen?"
        alert.informativeText = """
            Spanly will switch it into USB accessory mode and show your Mac's second screen on it. \
            Install the Spanly app on the device first. You can change the device later from the menu bar.
            """
        alert.addButton(withTitle: "Use as Second Screen")
        alert.addButton(withTitle: "Not Now")
        if alert.runModal() == .alertFirstButtonReturn { controller.choose(t) }
    }

    // MARK: Icon

    private func updateIcon() {
        let symbol: String
        if !Permissions.allGranted {
            symbol = "exclamationmark.triangle"
        } else if !controller.streamStatus.isEmpty {
            symbol = "rectangle.fill.on.rectangle.fill"
        } else {
            symbol = "rectangle.on.rectangle"
        }
        let image = NSImage(systemSymbolName: symbol, accessibilityDescription: "Spanly")
        image?.isTemplate = true
        item.button?.image = image
    }

    // MARK: Menu

    func menuNeedsUpdate(_ menu: NSMenu) {
        menu.removeAllItems()
        let settings = controller.settings

        // Status
        let streams = controller.streamStatus
        for s in streams {
            menu.addItem(label("Streaming to \(s.tablet)", bold: true))
            menu.addItem(label(s.detail))
        }
        if streams.isEmpty, controller.chosenSerial != nil {
            menu.addItem(label("Waiting for \(settings.deviceName ?? "the tablet")", bold: true))
            menu.addItem(label("Connect it with USB; Spanly opens on it by itself"))
        } else if streams.isEmpty {
            menu.addItem(label("No tablet chosen", bold: true))
            menu.addItem(label("Connect an Android tablet with USB"))
        }

        // Permissions
        if !Permissions.screenRecording {
            menu.addItem(.separator())
            menu.addItem(ActionItem("⚠︎ Allow Screen Recording…") { Permissions.requestScreenRecording() })
        }
        if !Permissions.accessibility {
            if Permissions.screenRecording { menu.addItem(.separator()) }
            menu.addItem(ActionItem("⚠︎ Allow Accessibility (for touch)…") { Permissions.requestAccessibility() })
        }

        if controller.primary?.link.isConnected == true {
            menu.addItem(.separator())
            if controller.sharingTablet {
                menu.addItem(ActionItem("Hide Tablet Screen") { [weak self] in self?.controller.hideTabletScreen() })
            } else {
                menu.addItem(ActionItem("Show Tablet Screen on Mac…") { [weak self] in self?.controller.showTabletScreen() })
            }
        }

        menu.addItem(.separator())
        menu.addItem(submenu("Tablet", tabletItems()))
        menu.addItem(submenu("Mode", Settings.Mode.allCases.map { m in
            ActionItem(m.title, checked: settings.mode == m) { [weak self] in
                guard settings.mode != m else { return }
                settings.mode = m
                self?.controller.applySettings(recreateDisplay: true)
            }
        }))
        menu.addItem(submenu("Resolution", Settings.Quality.allCases.map { q in
            ActionItem(q.title, checked: settings.quality == q) { [weak self] in
                guard settings.quality != q else { return }
                settings.quality = q
                self?.controller.applySettings(recreateDisplay: true)
            }
        }))
        menu.addItem(submenu("Position", Settings.Position.allCases.map { p in
            ActionItem(p.title, checked: settings.position == p) { [weak self] in
                settings.position = p
                self?.controller.applySettings(recreateDisplay: false)
            }
        }))
        menu.addItem(ActionItem("Allow Wi-Fi Connection", checked: settings.wifi) { [weak self] in
            self?.controller.setWifi(!settings.wifi)
        })
        if settings.wifi {
            menu.addItem(label("  Tablets plugged in once can then connect wirelessly"))
            menu.addItem(ActionItem("  Forget Paired Tablets") { [weak self] in self?.controller.forgetPairedTablets() })
        }
        menu.addItem(submenu("Sound", Settings.Sound.allCases.map { s in
            ActionItem(s.title, checked: settings.sound == s) { [weak self] in
                let capture = (settings.sound == .mac) != (s == .mac) // audio capture on/off
                settings.sound = s
                self?.controller.updateSpeakers()
                if capture { self?.controller.restartCapture() }
            }
        }))
        if TabletMicrophone.installed {
            menu.addItem(ActionItem("Use Tablet as Microphone", checked: settings.tabletMicrophone) { [weak self] in
                settings.tabletMicrophone.toggle()
                self?.controller.updateMicrophone()
            })
        } else {
            menu.addItem(ActionItem("Install Spanly Microphone…") {
                TabletMicrophone.install()
            })
        }
        menu.addItem(ActionItem("Return Pointer to Mac After Touch", checked: settings.restoreCursor) { [weak self] in
            settings.restoreCursor.toggle()
            self?.controller.applySettings(recreateDisplay: false)
        })

        menu.addItem(.separator())
        let login = SMAppService.mainApp.status
        menu.addItem(ActionItem("Open at Login", checked: login == .enabled || login == .requiresApproval) {
            if SMAppService.mainApp.status == .enabled {
                try? SMAppService.mainApp.unregister()
            } else {
                try? SMAppService.mainApp.register()
            }
        })
        menu.addItem(ActionItem("Show Log") { NSWorkspace.shared.open(logURL) })
        menu.addItem(.separator())
        menu.addItem(ActionItem("Quit Spanly", key: "q") { NSApp.terminate(nil) })
    }

    private func tabletItems() -> [NSMenuItem] {
        let chosen = controller.chosenSerial
        var items: [NSMenuItem] = controller.tablets.map { t in
            ActionItem(t.name, checked: t.serial == chosen) { [weak self] in self?.controller.choose(t) }
        }
        if let chosen, !items.contains(where: { $0.state == .on }) {
            items.insert(ActionItem("\(controller.settings.deviceName ?? chosen) (not connected)",
                                    checked: true, enabled: false) {}, at: 0)
        }
        if items.isEmpty {
            items.append(ActionItem("No Android devices connected", enabled: false) {})
        }
        return items
    }

    private func label(_ text: String, bold: Bool = false) -> NSMenuItem {
        let item = NSMenuItem(title: text, action: nil, keyEquivalent: "")
        item.isEnabled = false
        if bold {
            item.attributedTitle = NSAttributedString(string: text, attributes: [.font: NSFont.boldSystemFont(ofSize: 13)])
        }
        return item
    }

    private func submenu(_ title: String, _ items: [NSMenuItem]) -> NSMenuItem {
        let parent = NSMenuItem(title: title, action: nil, keyEquivalent: "")
        let sub = NSMenu()
        sub.autoenablesItems = false
        items.forEach(sub.addItem)
        parent.submenu = sub
        return parent
    }
}
