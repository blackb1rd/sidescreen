// Displays on Windows. Windows can't create a monitor without a driver: extend mode uses the
// Virtual Display Driver's monitor (github.com/VirtualDrivers/Virtual-Display-Driver), set to
// the tablet's resolution. Display ids are hashes of the monitor's device name (\\.\DISPLAY2).
#include "core/log.hpp"
#include "platform/platform.hpp"
#include "platform/windows/win.hpp"

#include <algorithm>
#include <vector>

namespace spanly::platform {

namespace {

struct Monitor {
    DisplayId id;
    std::wstring device;
    std::string description;
    RECT rect;
    bool primary;
};

DisplayId idFor(const std::wstring& device) {
    uint32_t hash = 2166136261U; // FNV-1a
    for (wchar_t c : device)
        hash = (hash ^ uint32_t(c)) * 16777619U;
    return hash;
}

std::vector<Monitor> monitors() {
    std::vector<Monitor> list;
    EnumDisplayMonitors(
        nullptr, nullptr,
        [](HMONITOR m, HDC, LPRECT, LPARAM out) -> BOOL {
            MONITORINFOEXW info{};
            info.cbSize = sizeof info;
            if (!GetMonitorInfoW(m, &info)) return TRUE;
            DISPLAY_DEVICEW dd{};
            dd.cb = sizeof dd;
            std::string description;
            if (EnumDisplayDevicesW(info.szDevice, 0, &dd, 0)) description = win::utf8(dd.DeviceString);
            reinterpret_cast<std::vector<Monitor>*>(out)->push_back({idFor(info.szDevice), info.szDevice, description,
                                                                     info.rcMonitor,
                                                                     (info.dwFlags & MONITORINFOF_PRIMARY) != 0});
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&list));
    return list;
}

std::optional<Monitor> monitor(DisplayId id) {
    for (auto& m : monitors()) {
        if (m.id == id) return m;
    }
    return std::nullopt;
}

bool setResolution(const std::wstring& device, int w, int h) {
    DEVMODEW mode{};
    mode.dmSize = sizeof mode;
    mode.dmPelsWidth = DWORD(w);
    mode.dmPelsHeight = DWORD(h);
    mode.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT;
    LONG r = ChangeDisplaySettingsExW(device.c_str(), &mode, nullptr, 0, nullptr);
    if (r != DISP_CHANGE_SUCCESSFUL) log("could not set the virtual monitor to {}x{} ({})", w, h, r);
    return r == DISP_CHANGE_SUCCESSFUL;
}

std::string lower(std::string s) {
    std::ranges::transform(s, s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

class WinVirtualDisplay : public VirtualDisplay {
public:
    WinVirtualDisplay(Monitor m, int w, int h) : m_(std::move(m)), w_(w), h_(h) {}
    DisplayId id() const override { return m_.id; }
    int tabletW() const override { return w_; }
    int tabletH() const override { return h_; }
    bool hiDPI() const override { return false; } // Windows scales by DPI itself
    bool resize(int w, int h) override {
        if (!setResolution(m_.device, w, h)) return false;
        w_ = w;
        h_ = h;
        return true;
    }
    void selectMode() override {}

    void place(const std::string& position, const std::vector<DisplayId>& others) override {
        if (position == "keep") return;
        auto me = monitor(m_.id);
        if (!me) return;
        RECT taken{};
        for (auto& m : monitors()) {
            bool other = m.primary || std::ranges::find(others, m.id) != others.end();
            if (m.id == m_.id || !other) continue;
            UnionRect(&taken, &taken, &m.rect);
        }
        LONG w = me->rect.right - me->rect.left, h = me->rect.bottom - me->rect.top;
        POINT origin = position == "left"    ? POINT{taken.left - w, 0}
                       : position == "above" ? POINT{0, taken.top - h}
                       : position == "below" ? POINT{0, taken.bottom}
                                             : POINT{taken.right, 0};
        DEVMODEW mode{};
        mode.dmSize = sizeof mode;
        mode.dmPosition = origin;
        mode.dmFields = DM_POSITION;
        ChangeDisplaySettingsExW(m_.device.c_str(), &mode, nullptr, CDS_UPDATEREGISTRY | CDS_NORESET, nullptr);
        ChangeDisplaySettingsExW(nullptr, nullptr, nullptr, 0, nullptr);
    }

private:
    Monitor m_;
    int w_, h_;
};

} // namespace

std::unique_ptr<VirtualDisplay> VirtualDisplay::create(int w, int h, int, bool, uint32_t) {
    auto list = monitors();
    auto isVirtual = [](const Monitor& m) {
        std::string d = lower(m.description);
        return !m.primary && (d.find("virtual") != std::string::npos || d.find("vdd") != std::string::npos ||
                              d.find("idd") != std::string::npos);
    };
    auto it = std::ranges::find_if(list, isVirtual);
    if (it == list.end()) {
        static bool told = false;
        if (!told) log("no virtual monitor found: install the Virtual Display Driver (see the README), or use Mirror");
        told = true;
        return nullptr;
    }
    setResolution(it->device, w, h);
    log("using {} as the second screen", it->description);
    return std::make_unique<WinVirtualDisplay>(*it, w, h);
}

DisplayId mainDisplay() {
    for (auto& m : monitors()) {
        if (m.primary) return m.id;
    }
    return 0;
}

std::optional<std::pair<int, int>> displayPixels(DisplayId id) {
    auto m = monitor(id);
    if (!m) return std::nullopt;
    DEVMODEW mode{};
    mode.dmSize = sizeof mode;
    if (!EnumDisplaySettingsW(m->device.c_str(), ENUM_CURRENT_SETTINGS, &mode)) return std::nullopt;
    return std::pair{int(mode.dmPelsWidth), int(mode.dmPelsHeight)};
}

Rect displayBounds(DisplayId id) {
    auto m = monitor(id);
    if (!m) return {};
    return {double(m->rect.left), double(m->rect.top), double(m->rect.right - m->rect.left),
            double(m->rect.bottom - m->rect.top)};
}

std::optional<DisplayId> displayNamed(const std::string& fragment) {
    for (auto& m : monitors()) {
        if (lower(m.description).find(lower(fragment)) != std::string::npos ||
            lower(win::utf8(m.device)).find(lower(fragment)) != std::string::npos)
            return m.id;
    }
    return std::nullopt;
}

Point cursorLocation() {
    POINT p{};
    GetCursorPos(&p);
    return {double(p.x), double(p.y)};
}

/// The monitor's device name for capture (\\.\DISPLAY2), or empty.
std::wstring displayDevice(DisplayId id) {
    auto m = monitor(id);
    return m ? m->device : std::wstring();
}

} // namespace spanly::platform
