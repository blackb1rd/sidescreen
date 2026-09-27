// Touch input from the tablet, already recognised as gestures by the app, turned into
// the mouse / scroll / keyboard events macOS understands (macOS has no touch input).
//
//   tap / drag            -> left click / drag (double-tap = double-click)
//   long press, 2-finger tap -> right click
//   2-finger drag         -> continuous (trackpad-style) scrolling, with momentum
//   pinch                 -> Cmd+= / Cmd+- in the app under the fingers

import AppKit

/// macOS has a single cursor, so a gesture borrows it: when the gesture ends the cursor
/// is put back where it was, leaving the MacBook screen's pointer undisturbed.
final class Pointer {
    enum Action: UInt8 {
        case down = 0, move = 1, up = 2, rightClick = 3
    }

    var displayID: CGDirectDisplayID = 0
    var restoreCursor = true
    private var savedCursor: CGPoint?
    private var borrowCount = 0
    private var isDown = false
    private var clickCount = 0
    private var lastDown = Date.distantPast
    private var lastDownPoint = CGPoint.zero
    private var scrollRemainder = (x: 0.0, y: 0.0)
    private var lastTouch = Date.distantPast

    /// True while a touch gesture is using the cursor (and briefly after), so the
    /// stream can leave the cursor out of the picture.
    var touchActive: Bool { borrowCount > 0 || Date().timeIntervalSince(lastTouch) < 0.6 }

    private var bounds: CGRect { CGDisplayBounds(displayID) }

    private func point(_ x: Float, _ y: Float) -> CGPoint {
        let b = bounds
        return CGPoint(x: b.minX + CGFloat(x) * b.width, y: b.minY + CGFloat(y) * b.height)
    }

    // MARK: Cursor borrowing

    private func borrowCursor() {
        borrowCount += 1
        guard restoreCursor, savedCursor == nil,
              let here = CGEvent(source: nil)?.location, !bounds.contains(here) else { return }
        savedCursor = here
    }

    private func returnCursor() {
        borrowCount = max(0, borrowCount - 1)
        guard borrowCount == 0, let back = savedCursor else { return }
        savedCursor = nil
        // Let the last event land before moving the cursor away.
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.05) {
            guard self.borrowCount == 0 else { return }
            CGWarpMouseCursorPosition(back)
            CGAssociateMouseAndMouseCursorPosition(1)
        }
    }

    // MARK: Clicks and drags

    func touch(action raw: UInt8, x: Float, y: Float) {
        guard let action = Action(rawValue: raw) else { return }
        lastTouch = Date()
        let p = point(x, y)
        switch action {
        case .down:
            borrowCursor()
            let now = Date()
            let near = hypot(p.x - lastDownPoint.x, p.y - lastDownPoint.y) < 12
            clickCount = (now.timeIntervalSince(lastDown) < NSEvent.doubleClickInterval && near) ? clickCount + 1 : 1
            lastDown = now
            lastDownPoint = p
            isDown = true
            mouse(.leftMouseDown, p, .left)
        case .move:
            mouse(isDown ? .leftMouseDragged : .mouseMoved, p, .left)
        case .up:
            guard isDown else { return }
            isDown = false
            mouse(.leftMouseUp, p, .left)
            returnCursor()
        case .rightClick:
            borrowCursor()
            clickCount = 1
            mouse(.rightMouseDown, p, .right)
            mouse(.rightMouseUp, p, .right)
            returnCursor()
        }
    }

    private func mouse(_ type: CGEventType, _ p: CGPoint, _ button: CGMouseButton) {
        guard let e = CGEvent(mouseEventSource: nil, mouseType: type, mouseCursorPosition: p, mouseButton: button) else { return }
        e.setIntegerValueField(.mouseEventClickState, value: Int64(max(clickCount, 1)))
        e.post(tap: .cghidEventTap)
    }

    // MARK: Pen

    /// A stylus behaves like a precise mouse: hovering moves the pointer (and leaves it on the
    /// tablet, like a real mouse would), touching presses the button with pressure, and the
    /// barrel button makes it a right button. action: 0 hover, 1 down, 2 move, 3 up.
    func pen(action: UInt8, buttons: UInt8, x: Float, y: Float, pressure: Float) {
        let p = point(x, y)
        let right = buttons & 1 != 0
        let button: CGMouseButton = right ? .right : .left
        let type: CGEventType
        switch action {
        case 0: type = .mouseMoved
        case 1:
            isDown = true
            let now = Date()
            let near = hypot(p.x - lastDownPoint.x, p.y - lastDownPoint.y) < 12
            clickCount = (now.timeIntervalSince(lastDown) < NSEvent.doubleClickInterval && near) ? clickCount + 1 : 1
            lastDown = now
            lastDownPoint = p
            type = right ? .rightMouseDown : .leftMouseDown
        case 2: type = isDown ? (right ? .rightMouseDragged : .leftMouseDragged) : .mouseMoved
        default:
            guard isDown else { return }
            isDown = false
            type = right ? .rightMouseUp : .leftMouseUp
        }
        guard let e = CGEvent(mouseEventSource: nil, mouseType: type, mouseCursorPosition: p, mouseButton: button) else { return }
        e.setIntegerValueField(.mouseEventClickState, value: Int64(max(clickCount, 1)))
        // Mark it as a tablet event so drawing apps read the pressure.
        e.setIntegerValueField(.mouseEventSubtype, value: Int64(CGEventMouseSubtype.tabletPoint.rawValue))
        e.setDoubleValueField(.mouseEventPressure, value: Double(action == 0 ? 0 : pressure))
        e.setDoubleValueField(.tabletEventPointPressure, value: Double(action == 0 ? 0 : pressure))
        e.post(tap: .cghidEventTap)
    }

    // MARK: Scrolling

    /// kind 0 = finger scrolling, 1 = momentum after the fingers lift.
    /// phase 1 = began, 2 = changed, 4 = ended. `last` = the whole gesture is over.
    /// Deltas are fractions of the tablet view, positive when the fingers move right/down.
    func scroll(kind: UInt8, phase: UInt8, last: Bool, x: Float, y: Float, dx: Float, dy: Float) {
        lastTouch = Date()
        let p = point(x, y)
        if kind == 0 && phase == 1 { scrollRemainder = (0, 0) }

        // Content follows the fingers, like scrolling on a trackpad with natural scrolling.
        let b = bounds
        let fx = Double(dx) * b.width + scrollRemainder.x
        let fy = Double(dy) * b.height + scrollRemainder.y
        let ix = Int32(fx.rounded(.towardZero)), iy = Int32(fy.rounded(.towardZero))
        scrollRemainder = (fx - Double(ix), fy - Double(iy))

        if let e = CGEvent(scrollWheelEvent2Source: nil, units: .pixel, wheelCount: 2, wheel1: iy, wheel2: ix, wheel3: 0) {
            e.setIntegerValueField(.scrollWheelEventIsContinuous, value: 1)
            if kind == 0 {
                e.setIntegerValueField(.scrollWheelEventScrollPhase, value: Int64(phase))
            } else {
                // NSEvent momentum phases: began 1, changed 2, ended 3.
                e.setIntegerValueField(.scrollWheelEventMomentumPhase, value: phase == 4 ? 3 : Int64(phase))
            }
            // Aim the event at the fingers without moving the cursor there: the window
            // server routes scroll events by the event's location.
            e.location = p
            e.post(tap: .cghidEventTap)
        }
    }

    // MARK: Zoom

    /// Pinch steps: Cmd+= (in) or Cmd+- (out), sent to the app whose window is under the fingers.
    func zoom(direction: Int8, x: Float, y: Float) {
        let p = point(x, y)
        activateApp(at: p)
        let key: CGKeyCode = direction > 0 ? 24 : 27 // kVK_ANSI_Equal, kVK_ANSI_Minus
        for down in [true, false] {
            guard let e = CGEvent(keyboardEventSource: nil, virtualKey: key, keyDown: down) else { continue }
            e.flags = .maskCommand
            e.post(tap: .cghidEventTap)
        }
    }

    private func activateApp(at p: CGPoint) {
        guard let windows = CGWindowListCopyWindowInfo([.optionOnScreenOnly, .excludeDesktopElements], kCGNullWindowID)
                as? [[String: Any]] else { return }
        for w in windows { // front to back
            guard (w[kCGWindowLayer as String] as? Int) == 0,
                  let bd = w[kCGWindowBounds as String] as? NSDictionary,
                  let rect = CGRect(dictionaryRepresentation: bd), rect.contains(p),
                  let pid = w[kCGWindowOwnerPID as String] as? pid_t else { continue }
            if NSWorkspace.shared.frontmostApplication?.processIdentifier != pid {
                NSRunningApplication(processIdentifier: pid)?.activate()
            }
            return
        }
    }
}
