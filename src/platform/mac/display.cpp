// Displays on macOS, and the virtual one sized to the tablet: the private CGVirtualDisplay API
// (macOS 11+, the same classes BetterDisplay and Chromium's test harness use).
#include "core/log.hpp"
#include "platform/mac/objc.hpp"
#include "platform/platform.hpp"

#include <CoreGraphics/CoreGraphics.h>
#include <dispatch/dispatch.h>

#include <algorithm>

namespace spanly::platform {

using namespace mac;

DisplayId mainDisplay() {
    return CGMainDisplayID();
}

std::optional<std::pair<int, int>> displayPixels(DisplayId id) {
    CGDisplayModeRef mode = CGDisplayCopyDisplayMode(id);
    if (!mode) return std::nullopt;
    std::pair<int, int> px{int(CGDisplayModeGetPixelWidth(mode)), int(CGDisplayModeGetPixelHeight(mode))};
    CGDisplayModeRelease(mode);
    return px;
}

Rect displayBounds(DisplayId id) {
    CGRect b = CGDisplayBounds(id);
    return {b.origin.x, b.origin.y, b.size.width, b.size.height};
}

Point cursorLocation() {
    CGEventRef e = CGEventCreate(nullptr);
    CGPoint p = CGEventGetLocation(e);
    CFRelease(e);
    return {p.x, p.y};
}

std::optional<DisplayId> displayNamed(const std::string& fragment) {
    Pool pool;
    Obj screens = send(cls("NSScreen"), "screens");
    auto n = send<unsigned long>(screens, "count");
    for (unsigned long i = 0; i < n; ++i) {
        Obj screen = send(screens, "objectAtIndex:", i);
        std::string name = toString(send(screen, "localizedName"));
        std::string a = name, b = fragment;
        std::ranges::transform(a, a.begin(), ::tolower);
        std::ranges::transform(b, b.begin(), ::tolower);
        if (a.find(b) == std::string::npos) continue;
        Obj number = send(send(screen, "deviceDescription"), "objectForKey:", nsString("NSScreenNumber"));
        return send<unsigned>(number, "unsignedIntValue");
    }
    return std::nullopt;
}

namespace {

class MacVirtualDisplay : public VirtualDisplay {
public:
    MacVirtualDisplay(Ref display, int w, int h, bool hiDPI)
        : display_(std::move(display)), w_(w), h_(h), hiDPI_(hiDPI) {}

    DisplayId id() const override { return send<unsigned>(display_.get(), "displayID"); }
    int tabletW() const override { return w_; }
    int tabletH() const override { return h_; }
    bool hiDPI() const override { return hiDPI_; }

    /// The desktop "looks like" half the tablet's resolution either way; with hiDPI macOS renders
    /// it at 2x (the tablet's full resolution), which is just sharper.
    static bool apply(Obj display, int w, int h, bool hiDPI) {
        Pool pool;
        Ref settings(make("CGVirtualDisplaySettings"));
        send<void>(settings.get(), "setHiDPI:", hiDPI ? 1U : 0U);
        Obj mode = send(send(send(cls("CGVirtualDisplayMode"), "alloc"),
                             "initWithWidth:height:refreshRate:", unsigned(w / 2), unsigned(h / 2), 60.0),
                        "autorelease");
        send<void>(settings.get(), "setModes:", send(cls("NSArray"), "arrayWithObject:", mode));
        return send<BOOL>(display, "applySettings:", settings.get());
    }

    bool resize(int w, int h) override {
        if (!apply(display_.get(), w, h, hiDPI_)) return false;
        w_ = w;
        h_ = h;
        return true;
    }

    void selectMode() override {
        auto w = size_t(w_ / 2), h = size_t(h_ / 2);                       // points
        size_t pw = hiDPI_ ? size_t(w_) : w, ph = hiDPI_ ? size_t(h_) : h; // pixels
        const void* keys[] = {kCGDisplayShowDuplicateLowResolutionModes};
        const void* values[] = {kCFBooleanTrue};
        CFDictionaryRef opts = CFDictionaryCreate(nullptr, keys, values, 1, &kCFTypeDictionaryKeyCallBacks,
                                                  &kCFTypeDictionaryValueCallBacks);
        CFArrayRef modes = CGDisplayCopyAllDisplayModes(id(), opts);
        CFRelease(opts);
        bool found = false;
        for (CFIndex i = 0; modes && i < CFArrayGetCount(modes) && !found; ++i) {
            auto mode = static_cast<CGDisplayModeRef>(const_cast<void*>(CFArrayGetValueAtIndex(modes, i)));
            if (CGDisplayModeGetWidth(mode) == w && CGDisplayModeGetHeight(mode) == h &&
                CGDisplayModeGetPixelWidth(mode) == pw && CGDisplayModeGetPixelHeight(mode) == ph) {
                CGDisplaySetDisplayMode(id(), mode, nullptr);
                found = true;
            }
        }
        if (modes) CFRelease(modes);
        if (!found) log("no {}x{} mode ({}x{} pixels) found for the virtual display", w, h, pw, ph);
    }

    void place(const std::string& position, const std::vector<DisplayId>& others) override {
        if (position == "keep") return;
        CGRect main = CGDisplayBounds(CGMainDisplayID());
        CGRect taken = main;
        for (DisplayId o : others) {
            if (o != id()) taken = CGRectUnion(taken, CGDisplayBounds(o));
        }
        CGRect me = CGDisplayBounds(id());
        CGPoint origin;
        if (position == "left") {
            origin = {taken.origin.x - me.size.width, main.origin.y};
        } else if (position == "above") {
            origin = {CGRectGetMidX(main) - me.size.width / 2, taken.origin.y - me.size.height};
        } else if (position == "below") {
            origin = {CGRectGetMidX(main) - me.size.width / 2, CGRectGetMaxY(taken)};
        } else {
            origin = {CGRectGetMaxX(taken), main.origin.y};
        }
        if (CGPointEqualToPoint(me.origin, origin)) return;
        CGDisplayConfigRef config = nullptr;
        if (CGBeginDisplayConfiguration(&config) != kCGErrorSuccess) return;
        CGConfigureDisplayOrigin(config, id(), int32_t(origin.x), int32_t(origin.y));
        if (CGCompleteDisplayConfiguration(config, kCGConfigurePermanently) != kCGErrorSuccess)
            log("could not move the virtual display");
    }

private:
    Ref display_;
    int w_, h_;
    bool hiDPI_;
};

} // namespace

std::unique_ptr<VirtualDisplay> VirtualDisplay::create(int w, int h, int dpi, bool hiDPI, uint32_t serial) {
    Pool pool;
    Ref d(make("CGVirtualDisplayDescriptor"));
    if (!d) return nullptr;
    send<void>(d.get(), "setQueue:", reinterpret_cast<Obj>(dispatch_get_main_queue()));
    send<void>(d.get(), "setName:", nsString("Spanly"));
    // HiDPI twins of a mode are only offered when the backing store may be 2x the mode; the
    // long side in both directions lets the same display turn portrait later.
    auto longSide = unsigned(std::max(w, h) * 2);
    send<void>(d.get(), "setMaxPixelsWide:", longSide);
    send<void>(d.get(), "setMaxPixelsHigh:", longSide);
    auto mm = [dpi](int px) { return double(px) / double(std::max(dpi, 1)) * 25.4; };
    send<void>(d.get(), "setSizeInMillimeters:", CGSizeMake(mm(w), mm(h)));
    send<void>(d.get(), "setVendorID:", 0x5344U); // "SD"
    send<void>(d.get(), "setProductID:", 0x0001U);
    send<void>(d.get(), "setSerialNum:", serial);
    void (^terminated)(Obj, Obj) = ^(Obj, Obj) {
      log("virtual display terminated");
    };
    send<void>(d.get(), "setTerminationHandler:", blockObject(terminated));
    Ref display(send(send(cls("CGVirtualDisplay"), "alloc"), "initWithDescriptor:", d.get()));
    if (!display || !MacVirtualDisplay::apply(display.get(), w, h, hiDPI)) return nullptr;
    return std::make_unique<MacVirtualDisplay>(std::move(display), w, h, hiDPI);
}

} // namespace spanly::platform
