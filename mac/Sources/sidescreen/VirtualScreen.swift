import AppKit
import CGVirtualDisplay

// MARK: - Virtual display

/// A display that exists only while SideScreen needs it (private CGVirtualDisplay API).
/// A fixed serial number lets macOS remember where you arranged it.
final class VirtualScreen {
    private let display: CGVirtualDisplay
    let tabletW: Int, tabletH: Int, hiDPI: Bool

    init?(tabletW: Int, tabletH: Int, dpi: Int, hiDPI: Bool) {
        let d = CGVirtualDisplayDescriptor()
        d.queue = DispatchQueue.main
        d.name = "SideScreen"
        // HiDPI twins of a mode are only offered when the backing store may be 2x the mode.
        d.maxPixelsWide = UInt32(tabletW * 2)
        d.maxPixelsHigh = UInt32(tabletH * 2)
        let mm = { (px: Int) in Double(px) / Double(max(dpi, 1)) * 25.4 }
        d.sizeInMillimeters = CGSize(width: mm(tabletW), height: mm(tabletH))
        d.vendorID = 0x5344 // "SD"
        d.productID = 0x0001
        d.serialNum = 0x0001
        d.terminationHandler = { _, _ in log("virtual display terminated") }
        guard let v = CGVirtualDisplay(descriptor: d) else { return nil }

        // The desktop "looks like" half the tablet's resolution either way; with hiDPI
        // macOS renders it at 2x (the tablet's full resolution), which is just sharper.
        let s = CGVirtualDisplaySettings()
        s.hiDPI = hiDPI ? 1 : 0
        s.modes = [CGVirtualDisplayMode(width: UInt32(tabletW / 2), height: UInt32(tabletH / 2), refreshRate: 60)]
        guard v.apply(s) else { return nil }

        display = v
        self.tabletW = tabletW
        self.tabletH = tabletH
        self.hiDPI = hiDPI
    }

    var displayID: CGDirectDisplayID { display.displayID }

    /// macOS may default to the low-resolution twin of a HiDPI mode; pick the 2x one explicitly.
    func selectHiDPIMode() {
        guard hiDPI else { return }
        let opts = [kCGDisplayShowDuplicateLowResolutionModes: true] as CFDictionary
        guard let modes = CGDisplayCopyAllDisplayModes(displayID, opts) as? [CGDisplayMode],
              let mode = modes.first(where: { $0.pixelWidth == tabletW && $0.width == tabletW / 2 }) else {
            log("no HiDPI mode found for the virtual display")
            return
        }
        CGDisplaySetDisplayMode(displayID, mode, nil)
    }

    /// Move the display next to the main one; macOS saves it like a manual arrangement.
    func place(_ position: String) {
        guard position != "keep" else { return }
        let main = CGDisplayBounds(CGMainDisplayID())
        let me = CGDisplayBounds(displayID)
        let origin: CGPoint
        switch position {
        case "left": origin = CGPoint(x: main.minX - me.width, y: main.minY)
        case "above": origin = CGPoint(x: main.midX - me.width / 2, y: main.minY - me.height)
        case "below": origin = CGPoint(x: main.midX - me.width / 2, y: main.maxY)
        default: origin = CGPoint(x: main.maxX, y: main.minY)
        }
        if me.origin == origin { return }
        var config: CGDisplayConfigRef?
        guard CGBeginDisplayConfiguration(&config) == .success, let config else { return }
        CGConfigureDisplayOrigin(config, displayID, Int32(origin.x), Int32(origin.y))
        if CGCompleteDisplayConfiguration(config, .permanently) != .success {
            log("could not move the virtual display")
        }
    }
}
