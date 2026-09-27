import AppKit

/// The tablet's microphone as a Mac microphone: the "SideScreen Microphone" audio driver (in
/// the app's Resources, installed once with an administrator password) shows up as an input
/// device; while "Use Tablet as Microphone" is on, the tablet's mic is played into it.
enum TabletMicrophone {
    static let deviceUID = "dev.blackb1rd.sidescreen.microphone"
    static let driverName = "SideScreenMicrophone.driver"
    private static let installDir = "/Library/Audio/Plug-Ins/HAL"

    static var installed: Bool { AudioDevices.id(forUID: deviceUID) != nil }

    /// Copy the driver into place and restart the audio service (asks for the admin password).
    static func install() {
        guard let driver = Bundle.main.resourceURL?.appendingPathComponent(driverName).path,
              FileManager.default.fileExists(atPath: driver) else {
            log("SideScreenMicrophone.driver is missing from the app bundle")
            return
        }
        let q = { (s: String) in "'" + s.replacingOccurrences(of: "'", with: "'\\''") + "'" }
        let command = "mkdir -p \(installDir) && rm -rf \(installDir)/\(driverName) && cp -R \(q(driver)) \(installDir)/ && killall coreaudiod"
        runAsAdmin(command, done: "installed the SideScreen Microphone driver")
    }

    static func uninstall() {
        runAsAdmin("rm -rf \(installDir)/\(driverName) && killall coreaudiod", done: "removed the SideScreen Microphone driver")
    }

    private static func runAsAdmin(_ command: String, done: String) {
        let script = "do shell script \"\(command.replacingOccurrences(of: "\\", with: "\\\\").replacingOccurrences(of: "\"", with: "\\\""))\" with administrator privileges"
        var error: NSDictionary?
        NSAppleScript(source: script)?.executeAndReturnError(&error)
        if let error {
            log("audio driver: \(error[NSAppleScript.errorMessage] ?? error)")
        } else {
            log(done)
        }
    }
}
