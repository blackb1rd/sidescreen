// Touch input from the tablet (already recognised as gestures by the app) as the mouse, scroll
// and keyboard events macOS understands (macOS has no touch input):
//
//   tap / drag               -> left click / drag (double-tap = double-click)
//   long press, 2-finger tap -> right click
//   2-finger drag            -> continuous (trackpad-style) scrolling, with momentum
//   pinch                    -> Cmd+= / Cmd+- in the app under the fingers
//   pen                      -> a precise mouse with pressure (a tablet event, for drawing apps)
//
// macOS has one cursor, so a gesture borrows it and puts it back when it ends. Main thread only.
#include "platform/mac/objc.hpp"
#include "platform/platform.hpp"

#include <ApplicationServices/ApplicationServices.h>

#include <chrono>
#include <cmath>
#include <dispatch/dispatch.h>

namespace spanly::platform {

using namespace mac;
using namespace std::chrono_literals;

namespace {

class MacPointer : public Pointer {
public:
    void touch(uint8_t action, float x, float y) override {
        lastTouch_ = Clock::now();
        CGPoint p = point(x, y);
        switch (action) {
        case 0: // down
            borrowCursor();
            countClick(p);
            down_ = true;
            mouse(kCGEventLeftMouseDown, p, kCGMouseButtonLeft);
            break;
        case 1: mouse(down_ ? kCGEventLeftMouseDragged : kCGEventMouseMoved, p, kCGMouseButtonLeft); break;
        case 2: // up
            if (!down_) return;
            down_ = false;
            mouse(kCGEventLeftMouseUp, p, kCGMouseButtonLeft);
            returnCursor();
            break;
        case 3: // right click
            borrowCursor();
            clicks_ = 1;
            mouse(kCGEventRightMouseDown, p, kCGMouseButtonRight);
            mouse(kCGEventRightMouseUp, p, kCGMouseButtonRight);
            returnCursor();
            break;
        default: break;
        }
    }

    /// action: 0 hover, 1 down, 2 move, 3 up. The barrel button makes it the right button.
    void pen(uint8_t action, uint8_t buttons, float x, float y, float pressure) override {
        CGPoint p = point(x, y);
        bool right = (buttons & 1U) != 0;
        CGEventType type;
        switch (action) {
        case 0: type = kCGEventMouseMoved; break;
        case 1:
            down_ = true;
            countClick(p);
            type = right ? kCGEventRightMouseDown : kCGEventLeftMouseDown;
            break;
        case 2:
            type = !down_ ? kCGEventMouseMoved : right ? kCGEventRightMouseDragged : kCGEventLeftMouseDragged;
            break;
        default:
            if (!down_) return;
            down_ = false;
            type = right ? kCGEventRightMouseUp : kCGEventLeftMouseUp;
        }
        CGEventRef e = CGEventCreateMouseEvent(nullptr, type, p, right ? kCGMouseButtonRight : kCGMouseButtonLeft);
        if (!e) return;
        CGEventSetIntegerValueField(e, kCGMouseEventClickState, std::max(clicks_, 1));
        CGEventSetIntegerValueField(e, kCGMouseEventSubtype, kCGEventMouseSubtypeTabletPoint);
        double pr = action == 0 ? 0 : pressure;
        CGEventSetDoubleValueField(e, kCGMouseEventPressure, pr);
        CGEventSetDoubleValueField(e, kCGTabletEventPointPressure, pr);
        CGEventPost(kCGHIDEventTap, e);
        CFRelease(e);
    }

    /// kind 0 = fingers, 1 = momentum after they lift; phase 1 began, 2 changed, 4 ended.
    /// Deltas are fractions of the tablet view, positive when the fingers move right/down.
    void scroll(uint8_t kind, uint8_t phase, bool, float x, float y, float dx, float dy) override {
        lastTouch_ = Clock::now();
        if (kind == 0 && phase == 1) remainder_ = {0, 0};
        CGRect b = CGDisplayBounds(display);
        // Content follows the fingers, like a trackpad with natural scrolling.
        double fx = double(dx) * b.size.width + remainder_.x;
        double fy = double(dy) * b.size.height + remainder_.y;
        auto ix = int32_t(std::trunc(fx)), iy = int32_t(std::trunc(fy));
        remainder_ = {fx - ix, fy - iy};
        CGEventRef e = CGEventCreateScrollWheelEvent2(nullptr, kCGScrollEventUnitPixel, 2, iy, ix, 0);
        if (!e) return;
        CGEventSetIntegerValueField(e, kCGScrollWheelEventIsContinuous, 1);
        if (kind == 0) {
            CGEventSetIntegerValueField(e, kCGScrollWheelEventScrollPhase, phase);
        } else { // NSEvent momentum phases: began 1, changed 2, ended 3
            CGEventSetIntegerValueField(e, kCGScrollWheelEventMomentumPhase, phase == 4 ? 3 : phase);
        }
        // Aim it at the fingers without moving the cursor: scroll events are routed by location.
        CGEventSetLocation(e, point(x, y));
        CGEventPost(kCGHIDEventTap, e);
        CFRelease(e);
    }

    /// Pinch steps: Cmd+= (in) or Cmd+- (out), to the app whose window is under the fingers.
    void zoom(int8_t direction, float x, float y) override {
        activateApp(point(x, y));
        CGKeyCode key = direction > 0 ? 24 : 27; // kVK_ANSI_Equal, kVK_ANSI_Minus
        for (bool down : {true, false}) {
            CGEventRef e = CGEventCreateKeyboardEvent(nullptr, key, down);
            if (!e) continue;
            CGEventSetFlags(e, kCGEventFlagMaskCommand);
            CGEventPost(kCGHIDEventTap, e);
            CFRelease(e);
        }
    }

    bool touchActive() const override { return borrowed_ > 0 || Clock::now() - lastTouch_ < 600ms; }

private:
    CGPoint point(float x, float y) const {
        CGRect b = CGDisplayBounds(display);
        return {b.origin.x + x * b.size.width, b.origin.y + y * b.size.height};
    }

    void countClick(CGPoint p) {
        auto now = Clock::now();
        bool near = std::hypot(p.x - lastDownPoint_.x, p.y - lastDownPoint_.y) < 12;
        auto interval = std::chrono::duration<double>(send<double>(cls("NSEvent"), "doubleClickInterval"));
        clicks_ = now - lastDown_ < interval && near ? clicks_ + 1 : 1;
        lastDown_ = now;
        lastDownPoint_ = p;
    }

    void mouse(CGEventType type, CGPoint p, CGMouseButton button) const {
        CGEventRef e = CGEventCreateMouseEvent(nullptr, type, p, button);
        if (!e) return;
        CGEventSetIntegerValueField(e, kCGMouseEventClickState, std::max(clicks_, 1));
        CGEventPost(kCGHIDEventTap, e);
        CFRelease(e);
    }

    void borrowCursor() {
        ++borrowed_;
        CGRect b = CGDisplayBounds(display);
        Point here = cursorLocation();
        if (!restoreCursor || saved_ || CGRectContainsPoint(b, {here.x, here.y})) return;
        saved_ = CGPoint{here.x, here.y};
    }

    void returnCursor() {
        borrowed_ = std::max(0, borrowed_ - 1);
        if (borrowed_ > 0 || !saved_) return;
        CGPoint back = *saved_;
        saved_.reset();
        // Let the last event land before moving the cursor away.
        runAfter(0.05, [this, back] {
            if (borrowed_ > 0) return;
            CGWarpMouseCursorPosition(back);
            CGAssociateMouseAndMouseCursorPosition(1);
        });
    }

    static void activateApp(CGPoint p) {
        Pool pool;
        CFArrayRef windows = CGWindowListCopyWindowInfo(
            kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements, kCGNullWindowID);
        if (!windows) return;
        for (CFIndex i = 0; i < CFArrayGetCount(windows); ++i) { // front to back
            auto w = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(windows, i));
            int layer = -1, pid = 0;
            CGRect r;
            auto layerRef = static_cast<CFNumberRef>(CFDictionaryGetValue(w, kCGWindowLayer));
            auto bounds = static_cast<CFDictionaryRef>(CFDictionaryGetValue(w, kCGWindowBounds));
            auto pidRef = static_cast<CFNumberRef>(CFDictionaryGetValue(w, kCGWindowOwnerPID));
            if (!layerRef || !bounds || !pidRef || !CFNumberGetValue(layerRef, kCFNumberIntType, &layer) ||
                layer != 0 || !CGRectMakeWithDictionaryRepresentation(bounds, &r) || !CGRectContainsPoint(r, p))
                continue;
            CFNumberGetValue(pidRef, kCFNumberIntType, &pid);
            Obj app = send(cls("NSRunningApplication"), "runningApplicationWithProcessIdentifier:", pid);
            Obj front = send(send(cls("NSWorkspace"), "sharedWorkspace"), "frontmostApplication");
            if (app && (!front || send<int>(front, "processIdentifier") != pid))
                send<BOOL>(app, "activateWithOptions:", 0UL);
            break;
        }
        CFRelease(windows);
    }

    bool down_ = false;
    int clicks_ = 0;
    int borrowed_ = 0;
    Clock::time_point lastDown_{}, lastTouch_{};
    CGPoint lastDownPoint_{};
    std::optional<CGPoint> saved_;
    struct {
        double x = 0, y = 0;
    } remainder_;
};

} // namespace

std::unique_ptr<Pointer> Pointer::create() {
    return std::make_unique<MacPointer>();
}

} // namespace spanly::platform
