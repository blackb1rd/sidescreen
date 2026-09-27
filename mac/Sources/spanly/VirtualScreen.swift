import AppKit
import CGVirtualDisplay

// MARK: - Virtual display

/// A display that exists only while Spanly needs it (private CGVirtualDisplay API).
/// A fixed serial number per tablet lets macOS remember where you arranged each one.
final class VirtualScreen {
    private let display: CGVirtualDisplay
    private(set) var tabletW: Int
    private(set) var tabletH: Int
    let hiDPI: Bool

    init?(tabletW: Int, tabletH: Int, dpi: Int, hiDPI: Bool, serial: UInt32 = 1) {
        let d = CGVirtualDisplayDescriptor()
        d.queue = DispatchQueue.main
        d.name = "Spanly"
        // HiDPI twins of a mode are only offered when the backing store may be 2x the mode;
        // the long side in both directions lets the same display turn portrait later.
        let longSide = UInt32(max(tabletW, tabletH) * 2)
        d.maxPixelsWide = longSide
        d.maxPixelsHigh = longSide
        let mm = { (px: Int) in Double(px) / Double(max(dpi, 1)) * 25.4 }
        d.sizeInMillimeters = CGSize(width: mm(tabletW), height: mm(tabletH))
        d.vendorID = 0x5344 // "SD"
        d.productID = 0x0001
        d.serialNum = serial
        d.terminationHandler = { _, _ in log("virtual display terminated") }
        guard let v = CGVirtualDisplay(descriptor: d) else { return nil }

        // The desktop "looks like" half the tablet's resolution either way; with hiDPI
        // macOS renders it at 2x (the tablet's full resolution), which is just sharper.
        guard v.apply(VirtualScreen.settings(tabletW, tabletH, hiDPI)) else { return nil }

        display = v
        self.tabletW = tabletW
        self.tabletH = tabletH
        self.hiDPI = hiDPI
    }

    var displayID: CGDirectDisplayID { display.displayID }

    private static func settings(_ w: Int, _ h: Int, _ hiDPI: Bool) -> CGVirtualDisplaySettings {
        let s = CGVirtualDisplaySettings()
        s.hiDPI = hiDPI ? 1 : 0
        s.modes = [CGVirtualDisplayMode(width: UInt32(w / 2), height: UInt32(h / 2), refreshRate: 60)]
        return s
    }

    /// Reshape the display in place (e.g. the tablet turned portrait), keeping its windows.
    func resize(tabletW w: Int, tabletH h: Int) -> Bool {
        guard display.apply(VirtualScreen.settings(w, h, hiDPI)) else { return false }
        tabletW = w
        tabletH = h
        return true
    }

    /// Select the mode matching the tablet: macOS may otherwise pick another generated mode
    /// (e.g. 1600x1200 for a 1600x2560 tablet) or the low-resolution twin of a HiDPI mode.
    func selectMode() {
        let (w, h) = (tabletW / 2, tabletH / 2) // points
        let (pw, ph) = hiDPI ? (tabletW, tabletH) : (w, h) // pixels
        let opts = [kCGDisplayShowDuplicateLowResolutionModes: true] as CFDictionary
        guard let modes = CGDisplayCopyAllDisplayModes(displayID, opts) as? [CGDisplayMode],
              let mode = modes.first(where: {
                  $0.width == w && $0.height == h && $0.pixelWidth == pw && $0.pixelHeight == ph
              }) else {
            log("no \(w)x\(h) mode (\(pw)x\(ph) pixels) found for the virtual display")
            return
        }
        CGDisplaySetDisplayMode(displayID, mode, nil)
    }

    /// Move the display next to the main one (beyond `others`, the other tablets' displays);
    /// macOS saves it like a manual arrangement.
    func place(_ position: String, others: [CGDirectDisplayID] = []) {
        guard position != "keep" else { return }
        let main = CGDisplayBounds(CGMainDisplayID())
        let taken = others.filter { $0 != displayID }.map { CGDisplayBounds($0) }.reduce(main) { $0.union($1) }
        let me = CGDisplayBounds(displayID)
        let origin: CGPoint
        switch position {
        case "left": origin = CGPoint(x: taken.minX - me.width, y: main.minY)
        case "above": origin = CGPoint(x: main.midX - me.width / 2, y: taken.minY - me.height)
        case "below": origin = CGPoint(x: main.midX - me.width / 2, y: taken.maxY)
        default: origin = CGPoint(x: taken.maxX, y: main.minY)
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
