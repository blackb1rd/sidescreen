import AppKit

/// `spanly://` URLs, for Shortcuts and scripts:
///   spanly://show-tablet   show the tablet's screen in a window on the Mac
///   spanly://hide-tablet   close that window
final class URLHandler: NSObject {
    private let controller: Controller

    init(controller: Controller) {
        self.controller = controller
        super.init()
        NSAppleEventManager.shared().setEventHandler(
            self, andSelector: #selector(handle(_:reply:)),
            forEventClass: AEEventClass(kInternetEventClass), andEventID: AEEventID(kAEGetURL))
    }

    @objc private func handle(_ event: NSAppleEventDescriptor, reply: NSAppleEventDescriptor) {
        guard let text = event.paramDescriptor(forKeyword: keyDirectObject)?.stringValue,
              let url = URL(string: text), url.scheme == "spanly" else { return }
        switch url.host {
        case "show-tablet": controller.showTabletScreen()
        case "hide-tablet": controller.hideTabletScreen()
        default: log("unknown URL \(text)")
        }
    }
}
