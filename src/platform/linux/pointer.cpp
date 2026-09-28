// Touch input on Linux, through the desktop portal's RemoteDesktop session (evdev codes).
#include "platform/linux/portal.hpp"
#include "platform/platform.hpp"

#include <chrono>

namespace spanly::platform {

namespace {

constexpr int kButtonLeft = 0x110, kButtonRight = 0x111;
constexpr int kKeyLeftCtrl = 29, kKeyEqual = 13, kKeyMinus = 12;

class PortalPointer : public Pointer {
public:
    void touch(uint8_t action, float x, float y) override {
        auto* s = session();
        if (!s) return;
        lastTouch_ = Clock::now();
        moveTo(*s, x, y);
        if (action == 0) {
            down_ = true;
            s->button(kButtonLeft, true);
        } else if (action == 2 && down_) {
            down_ = false;
            s->button(kButtonLeft, false);
        } else if (action == 3) {
            s->button(kButtonRight, true);
            s->button(kButtonRight, false);
        }
    }

    void pen(uint8_t action, uint8_t buttons, float x, float y, float) override {
        // A precise mouse: hover moves, touching presses (the barrel button: the right one).
        auto* s = session();
        if (!s) return;
        moveTo(*s, x, y);
        int button = (buttons & 1U) ? kButtonRight : kButtonLeft;
        if (action == 1) {
            down_ = true;
            s->button(button, true);
        } else if (action == 3 && down_) {
            down_ = false;
            s->button(button, false);
        }
    }

    /// Portal axis values scroll the view; content following the fingers is the opposite sign.
    void scroll(uint8_t, uint8_t, bool, float x, float y, float dx, float dy) override {
        auto* s = session();
        if (!s) return;
        lastTouch_ = Clock::now();
        moveTo(*s, x, y);
        s->axis(-double(dx) * s->width(), -double(dy) * s->height());
    }

    void zoom(int8_t direction, float, float) override {
        auto* s = session();
        if (!s) return;
        int key = direction > 0 ? kKeyEqual : kKeyMinus;
        s->key(kKeyLeftCtrl, true);
        s->key(key, true);
        s->key(key, false);
        s->key(kKeyLeftCtrl, false);
    }

    bool touchActive() const override { return Clock::now() - lastTouch_ < std::chrono::milliseconds(600); }

private:
    portal::PortalSession* session() const { return portal::sessionFor(display); }
    static void moveTo(const portal::PortalSession& s, float x, float y) {
        s.pointerTo(double(x) * s.width(), double(y) * s.height());
    }

    bool down_ = false;
    Clock::time_point lastTouch_;
};

} // namespace

std::unique_ptr<Pointer> Pointer::create() {
    return std::make_unique<PortalPointer>();
}

} // namespace spanly::platform
