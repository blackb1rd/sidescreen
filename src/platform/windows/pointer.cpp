// Touch input on Windows: mouse input with SendInput, positioned on the streamed monitor.
#include "platform/platform.hpp"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <vector>

namespace spanly::platform {

namespace {

constexpr float kWheelPerPixel = 2.0F; // wheel units per pixel of finger movement (120 = one notch)

class WinPointer : public Pointer {
public:
    void touch(uint8_t action, float x, float y) override {
        lastTouch_ = Clock::now();
        std::vector<INPUT> in{moveTo(x, y)};
        if (action == 0) {
            down_ = true;
            in.push_back(mouse(MOUSEEVENTF_LEFTDOWN));
        } else if (action == 2 && down_) {
            down_ = false;
            in.push_back(mouse(MOUSEEVENTF_LEFTUP));
        } else if (action == 3) {
            in.push_back(mouse(MOUSEEVENTF_RIGHTDOWN));
            in.push_back(mouse(MOUSEEVENTF_RIGHTUP));
        }
        send(in);
    }

    void pen(uint8_t action, uint8_t buttons, float x, float y, float) override {
        bool right = (buttons & 1U) != 0;
        std::vector<INPUT> in{moveTo(x, y)};
        if (action == 1) {
            down_ = true;
            in.push_back(mouse(right ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_LEFTDOWN));
        } else if (action == 3 && down_) {
            down_ = false;
            in.push_back(mouse(right ? MOUSEEVENTF_RIGHTUP : MOUSEEVENTF_LEFTUP));
        }
        send(in);
    }

    /// Content follows the fingers: fingers down = wheel up, fingers right = wheel left.
    void scroll(uint8_t, uint8_t, bool, float x, float y, float dx, float dy) override {
        lastTouch_ = Clock::now();
        Rect b = displayBounds(display);
        auto v = int(dy * float(b.h) * kWheelPerPixel), h = -int(dx * float(b.w) * kWheelPerPixel);
        std::vector<INPUT> in{moveTo(x, y)};
        if (v) in.push_back(mouse(MOUSEEVENTF_WHEEL, v));
        if (h) in.push_back(mouse(MOUSEEVENTF_HWHEEL, h));
        send(in);
    }

    /// Ctrl + plus / minus.
    void zoom(int8_t direction, float, float) override {
        WORD k = direction > 0 ? VK_OEM_PLUS : VK_OEM_MINUS;
        send({key(VK_CONTROL, false), key(k, false), key(k, true), key(VK_CONTROL, true)});
    }

    bool touchActive() const override { return Clock::now() - lastTouch_ < std::chrono::milliseconds(600); }

private:
    static INPUT mouse(DWORD flags, int data = 0, LONG dx = 0, LONG dy = 0) {
        INPUT i{};
        i.type = INPUT_MOUSE;
        i.mi.dx = dx;
        i.mi.dy = dy;
        i.mi.mouseData = DWORD(data);
        i.mi.dwFlags = flags;
        return i;
    }

    static INPUT key(WORD vk, bool up) {
        INPUT i{};
        i.type = INPUT_KEYBOARD;
        i.ki.wVk = vk;
        i.ki.dwFlags = up ? KEYEVENTF_KEYUP : 0;
        return i;
    }

    static void send(std::vector<INPUT> in) { SendInput(UINT(in.size()), in.data(), sizeof(INPUT)); }

    /// Absolute coordinates (0-65535 across the whole virtual desktop) for a point on the monitor.
    INPUT moveTo(float x, float y) const {
        Rect b = displayBounds(display);
        double px = b.x + x * b.w, py = b.y + y * b.h;
        double vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
        double vw = std::max(1, GetSystemMetrics(SM_CXVIRTUALSCREEN)),
               vh = std::max(1, GetSystemMetrics(SM_CYVIRTUALSCREEN));
        return mouse(MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK, 0, LONG((px - vx) * 65535 / vw),
                     LONG((py - vy) * 65535 / vh));
    }

    bool down_ = false;
    Clock::time_point lastTouch_{};
};

} // namespace

std::unique_ptr<Pointer> Pointer::create() {
    return std::make_unique<WinPointer>();
}

} // namespace spanly::platform
