import Foundation

/// The tablet's microphone as "SideScreen Microphone". Main thread only.
extension Controller {
    /// Start or stop the first tablet's microphone to match the setting and the connections.
    func updateMicrophone() {
        let enabled = settings.tabletMicrophone && TabletMicrophone.installed
        let target = enabled ? primary.flatMap { $0.link.isConnected ? $0 : nil } : nil
        guard target !== micSession || (target != nil) != (micPlayer != nil) else { return }
        if let old = micSession {
            old.link.send(.micStop, Data())
            micSession = nil
        }
        micPlayer?.stop()
        micPlayer = nil
        guard let s = target else { return }
        guard let player = PCMPlayer(channels: 1, deviceUID: TabletMicrophone.deviceUID) else {
            log("could not open SideScreen Microphone")
            return
        }
        micPlayer = player
        micSession = s
        s.link.send(.micStart, Data())
        log("using \(s.name)'s microphone as SideScreen Microphone")
    }
}
