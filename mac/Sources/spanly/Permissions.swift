// The two macOS permissions Spanly needs, and the first-launch guide for them.
//
//   Screen Recording — to capture the virtual display (without it the tablet stays black)
//   Accessibility    — to turn touches into clicks and scrolls (without it touch does nothing)

import AppKit
import ApplicationServices

enum Permissions {
    static var screenRecording: Bool { CGPreflightScreenCaptureAccess() }
    static var accessibility: Bool { AXIsProcessTrusted() }
    static var allGranted: Bool { screenRecording && accessibility }

    /// Adds Spanly to the Screen Recording list (asking once) and opens that settings page.
    static func requestScreenRecording() {
        if !CGRequestScreenCaptureAccess() { openSettings("Privacy_ScreenCapture") }
    }

    /// Adds Spanly to the Accessibility list (asking once) and opens that settings page.
    static func requestAccessibility() {
        let prompt = [kAXTrustedCheckOptionPrompt.takeUnretainedValue(): true] as CFDictionary
        if !AXIsProcessTrustedWithOptions(prompt) { openSettings("Privacy_Accessibility") }
    }

    static func openSettings(_ anchor: String) {
        if let url = URL(string: "x-apple.systempreferences:com.apple.preference.security?\(anchor)") {
            NSWorkspace.shared.open(url)
        }
    }

    /// Explain what is missing and why, then open the settings for it.
    static func showGuide() {
        guard !allGranted else { return }
        var missing: [String] = []
        if !screenRecording { missing.append("• Screen Recording — to show your Mac screen on the tablet") }
        if !accessibility { missing.append("• Accessibility — to turn touches on the tablet into clicks and scrolls") }

        NSApp.activate(ignoringOtherApps: true)
        let alert = NSAlert()
        alert.messageText = "Spanly needs your permission"
        alert.informativeText = """
            \(missing.joined(separator: "\n"))

            Turn Spanly on in the System Settings pages that open next. \
            If macOS asks to quit and reopen Spanly, choose Quit & Reopen.
            """
        alert.addButton(withTitle: "Open System Settings")
        alert.addButton(withTitle: "Later")
        guard alert.runModal() == .alertFirstButtonReturn else { return }
        if !screenRecording { requestScreenRecording() }
        if !accessibility { requestAccessibility() }
    }
}
