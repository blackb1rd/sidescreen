import AppKit

/// Showing the tablet's own screen in a Mac window, and controlling it from there.
/// Main thread only (link callbacks hop to the main queue).
extension Controller {
    var sharingTablet: Bool { tabletWindow != nil }

    func showTabletScreen() {
        guard link.isConnected else { return }
        if let w = tabletWindow { return w.show() }
        let w = TabletWindow(title: usb?.connected?.name ?? settings.deviceName ?? "Tablet")
        w.view.send = { [weak self] type, payload in self?.link.send(type, payload) }
        w.onClose = { [weak self] in
            self?.link.send(.shareStop, Data())
            self?.tabletWindow = nil
        }
        tabletWindow = w
        link.send(.shareStart, Data())
        log("asked the tablet to share its screen")
    }

    func hideTabletScreen() {
        tabletWindow?.window.close() // windowWillClose sends shareStop
    }

    func wireTabletShare(_ l: Link) {
        l.onShareSize = { [weak self] w, h in
            DispatchQueue.main.async {
                guard let win = self?.tabletWindow else { return }
                win.setSize(width: w, height: h)
                win.show()
            }
        }
        l.onShareConfig = { [weak self] data in self?.tabletWindow?.config(data) }
        l.onShareFrame = { [weak self] data in self?.tabletWindow?.frame(data) }
        l.onShareStatus = { [weak self] state, control in
            DispatchQueue.main.async {
                guard let self, let win = self.tabletWindow else { return }
                switch state {
                case 1: win.setControlAvailable(control)
                case 2:
                    log("the tablet declined to share its screen")
                    self.tabletWindow = nil
                    win.window.close()
                default:
                    self.tabletWindow = nil
                    win.window.close()
                }
            }
        }
        l.onViewing = { [weak self] viewing in
            DispatchQueue.main.async { self?.setTabletViewing(viewing) }
        }
    }
}
