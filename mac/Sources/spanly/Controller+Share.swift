import AppKit

/// Showing the tablet's own screen in a Mac window, and controlling it from there.
/// Main thread only (link callbacks hop to the main queue).
extension Controller {
    var sharingTablet: Bool { tabletWindow != nil }

    func showTabletScreen() {
        guard let s = primary, s.link.isConnected else { return }
        if let w = tabletWindow { return w.show() }
        let w = TabletWindow(title: s.name)
        w.view.send = { [weak s] type, payload in s?.link.send(type, payload) }
        w.onClose = { [weak self, weak s] in
            s?.link.send(.shareStop, Data())
            self?.tabletWindow = nil
        }
        tabletWindow = w
        shareSession = s
        s.link.send(.shareStart, Data())
        log("asked the tablet to share its screen")
    }

    func hideTabletScreen() {
        tabletWindow?.window.close() // windowWillClose sends shareStop
    }

    func wireTabletShare(_ l: Link) {
        // Only the tablet being shown reaches the window, and only the microphone's tablet the player.
        let window = { [weak self, weak l] () -> TabletWindow? in
            guard let self, let s = l?.session, s === self.shareSession else { return nil }
            return self.tabletWindow
        }
        l.onShareSize = { w, h in
            DispatchQueue.main.async {
                guard let win = window() else { return }
                win.setSize(width: w, height: h)
                win.show()
            }
        }
        l.onShareConfig = { data in window()?.config(data) }
        l.onShareFrame = { data in window()?.frame(data) }
        l.onShareAudio = { pcm in window()?.audio(pcm) }
        l.onMicAudio = { [weak self, weak l] pcm in
            guard let self, let s = l?.session, s === self.micSession else { return }
            self.micPlayer?.play(pcm)
        }
        l.onShareStatus = { [weak self] state, control in
            DispatchQueue.main.async {
                guard let self, let win = window() else { return }
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
    }
}
