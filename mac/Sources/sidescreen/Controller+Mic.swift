import Foundation

/// The tablet's microphone as "SideScreen Microphone". Main thread only.
extension Controller {
    /// Start or stop the tablet's microphone to match the setting and the connection.
    func updateMicrophone() {
        let want = settings.tabletMicrophone && link.isConnected && TabletMicrophone.installed
        if want && micPlayer == nil {
            guard let player = PCMPlayer(channels: 1, deviceUID: TabletMicrophone.deviceUID) else {
                log("could not open SideScreen Microphone")
                return
            }
            micPlayer = player
            link.send(.micStart, Data())
            log("using the tablet's microphone as SideScreen Microphone")
        } else if !want, let player = micPlayer {
            if link.isConnected { link.send(.micStop, Data()) }
            player.stop()
            micPlayer = nil
        }
    }
}
