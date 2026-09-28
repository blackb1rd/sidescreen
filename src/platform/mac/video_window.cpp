// A window on the Mac showing the tablet's own screen (the reverse direction): H.264 from the
// tablet shown by an AVSampleBufferDisplayLayer (hardware decoding), and mouse and keyboard
// input turned into taps, swipes and typing on the tablet.
#include "platform/mac/objc.hpp"
#include "platform/platform.hpp"

#include <CoreGraphics/CoreGraphics.h>
#include <CoreMedia/CoreMedia.h>
#include <dispatch/dispatch.h>

#include <algorithm>
#include <iterator>
#include <utility>

namespace spanly::platform {

using namespace mac;

namespace {

class MacVideoWindow;

// Mac key codes that map to Android key codes.
constexpr std::pair<unsigned short, uint16_t> kKeys[] = {
    {36, 66},   {76, 66},                        // return, enter -> KEYCODE_ENTER
    {51, 67},                                    // delete -> KEYCODE_DEL
    {117, 112},                                  // forward delete
    {48, 61},                                    // tab
    {123, 21},  {124, 22}, {125, 20}, {126, 19}, // arrows
};

uint8_t nalType(ByteView nal) {
    return nal.empty() ? 0 : uint8_t(nal[0] & 0x1FU);
}

class MacVideoWindow : public VideoWindow {
public:
    MacVideoWindow(const std::string& title, Input input) : input_(std::move(input)) {
        Pool pool;
        queue_ = dispatch_queue_create("spanly.tablet-video", DISPATCH_QUEUE_SERIAL);
        CGRect frame = CGRectMake(0, 0, 900, 563);
        window_ = Ref(send(send(cls("NSWindow"), "alloc"), "initWithContentRect:styleMask:backing:defer:", frame,
                           15UL /* titled, closable, miniaturizable, resizable */, 2UL /* buffered */, NO));
        send<void>(window_.get(), "setTitle:", nsString(title));
        send<void>(window_.get(), "setReleasedWhenClosed:", NO);
        view_ = Ref(send(instance(viewClass(), this, false), "initWithFrame:", frame));
        send<void>(view_.get(), "setWantsLayer:", YES);
        Obj layer = send(cls("CALayer"), "layer");
        send<void>(layer, "setBackgroundColor:", CGColorGetConstantColor(kCGColorBlack));
        send<void>(view_.get(), "setLayer:", layer);
        display_ = Ref(make("AVSampleBufferDisplayLayer"));
        send<void>(display_.get(), "setVideoGravity:", nsString("AVLayerVideoGravityResizeAspect"));
        send<void>(layer, "addSublayer:", display_.get());
        send<void>(window_.get(), "setContentView:", view_.get());
        send<void>(window_.get(), "setDelegate:", view_.get());
        addNavigationButtons();
        send<void>(window_.get(), "center");
    }

    ~MacVideoWindow() override {
        send<void>(window_.get(), "setDelegate:", nullptr);
        send<void>(window_.get(), "close");
    }

    void show() override {
        send<void>(send(cls("NSApplication"), "sharedApplication"), "activateIgnoringOtherApps:", YES);
        send<void>(window_.get(), "makeKeyAndOrderFront:", nullptr);
        send<BOOL>(window_.get(), "makeFirstResponder:", view_.get());
    }

    void close() override { send<void>(window_.get(), "close"); }

    void setControlAvailable(bool available) override {
        Pool pool;
        send<void>(window_.get(), "setSubtitle:",
                   nsString(available ? ""
                                      : "To control it from here, turn on “Spanly: control from Mac” in the "
                                        "tablet's Settings › Accessibility"));
    }

    /// New stream geometry: shape the window like the tablet's screen.
    void setVideoSize(int w, int h) override {
        videoW_ = w;
        videoH_ = h;
        send<void>(window_.get(), "setContentAspectRatio:", CGSizeMake(w, h));
        Obj screen = send(window_.get(), "screen");
        if (!screen) screen = send(cls("NSScreen"), "mainScreen");
        double maxH = screen ? send<CGRect>(screen, "visibleFrame").size.height * 0.8 : 700;
        double height = std::min(maxH, double(h));
        send<void>(window_.get(), "setContentSize:", CGSizeMake(height * w / h, height));
        dispatch_async(queue_, ^{
          clearFormat(); // wait for the new parameter sets
        });
    }

    void config(const Bytes& annexB) override {
        Bytes copy = annexB; // NOLINT(performance-unnecessary-copy-initialization): the block needs its own
        dispatch_async(queue_, ^{
          updateFormat(nalUnits(copy));
        });
    }

    void frame(const Bytes& annexB) override {
        Bytes copy = annexB; // NOLINT(performance-unnecessary-copy-initialization): the block needs its own
        dispatch_async(queue_, ^{
          decode(copy);
        });
    }

private:
    void clearFormat() {
        if (format_) CFRelease(format_);
        format_ = nullptr;
    }

    void updateFormat(const std::vector<ByteView>& nals) {
        auto sps = std::ranges::find_if(nals, [](ByteView n) { return nalType(n) == 7; });
        auto pps = std::ranges::find_if(nals, [](ByteView n) { return nalType(n) == 8; });
        if (sps == nals.end() || pps == nals.end()) return;
        const uint8_t* sets[] = {sps->data(), pps->data()};
        size_t sizes[] = {sps->size(), pps->size()};
        CMVideoFormatDescriptionRef fmt = nullptr;
        if (CMVideoFormatDescriptionCreateFromH264ParameterSets(nullptr, 2, sets, sizes, 4, &fmt) != noErr) return;
        clearFormat();
        format_ = fmt;
    }

    void decode(const Bytes& annexB) {
        auto nals = nalUnits(annexB);
        if (std::ranges::any_of(nals, [](ByteView n) { return nalType(n) == 7; })) updateFormat(nals);
        if (!format_) return;
        // AVCC: each NAL unit with its 4-byte length; parameter sets and delimiters dropped.
        Bytes avcc;
        for (ByteView nal : nals) {
            uint8_t t = nalType(nal);
            if (t == 7 || t == 8 || t == 9) continue;
            putU32(avcc, uint32_t(nal.size()));
            avcc.insert(avcc.end(), nal.begin(), nal.end());
        }
        if (avcc.empty()) return;
        CMBlockBufferRef block = nullptr;
        if (CMBlockBufferCreateWithMemoryBlock(nullptr, nullptr, avcc.size(), nullptr, nullptr, 0, avcc.size(),
                                               kCMBlockBufferAssureMemoryNowFlag, &block) != noErr)
            return;
        CMBlockBufferReplaceDataBytes(avcc.data(), block, 0, avcc.size());
        CMSampleBufferRef sb = nullptr;
        size_t size = avcc.size();
        OSStatus s = CMSampleBufferCreateReady(nullptr, block, format_, 1, 0, nullptr, 1, &size, &sb);
        CFRelease(block);
        if (s != noErr || !sb) return;
        // Show each frame as soon as it's decoded; there is no playback clock here.
        if (CFArrayRef atts = CMSampleBufferGetSampleAttachmentsArray(sb, true); atts && CFArrayGetCount(atts) > 0) {
            auto dict = (CFMutableDictionaryRef)CFArrayGetValueAtIndex(atts, 0);
            CFDictionarySetValue(dict, kCMSampleAttachmentKey_DisplayImmediately, kCFBooleanTrue);
        }
        Obj renderer = send(display_.get(), "sampleBufferRenderer");
        if (send<long>(renderer, "status") == 2) send<void>(renderer, "flush"); // failed
        send<void>(renderer, "enqueueSampleBuffer:", sb);
        CFRelease(sb);
    }

    /// Position within the letterboxed video as fractions of the tablet's screen.
    std::pair<float, float> position(Obj event) const {
        auto p =
            send<CGPoint>(view_.get(), "convertPoint:fromView:", send<CGPoint>(event, "locationInWindow"), nullptr);
        CGRect r = videoRect();
        if (r.size.width <= 0 || r.size.height <= 0) return {0, 0};
        auto x = float((p.x - r.origin.x) / r.size.width);
        auto y = float(1 - (p.y - r.origin.y) / r.size.height); // AppKit's y axis points up
        return {std::clamp(x, 0.0F, 1.0F), std::clamp(y, 0.0F, 1.0F)};
    }

    CGRect videoRect() const {
        auto b = send<CGRect>(view_.get(), "bounds");
        if (videoW_ <= 0 || videoH_ <= 0) return b;
        double scale = std::min(b.size.width / videoW_, b.size.height / videoH_);
        double w = videoW_ * scale, h = videoH_ * scale;
        return CGRectMake(b.origin.x + (b.size.width - w) / 2, b.origin.y + (b.size.height - h) / 2, w, h);
    }

    void pointer(uint8_t action, Obj event) const {
        auto [x, y] = position(event);
        if (input_.pointer) input_.pointer(action, x, y);
    }

    void scroll(Obj event) const {
        CGRect r = videoRect();
        if (r.size.width <= 0 || !input_.scroll) return;
        double line = send<BOOL>(event, "hasPreciseScrollingDeltas") ? 1 : 12; // mouse wheels scroll in lines
        auto [x, y] = position(event);
        input_.scroll(x, y, float(send<double>(event, "scrollingDeltaX") * line / r.size.width),
                      float(send<double>(event, "scrollingDeltaY") * line / r.size.height));
    }

    /// REMOTE_KEY: kind 0 text, 1 Android key code, 2 global action (1 back, 2 home, 3 recents).
    bool key(Obj event) const {
        if (send<unsigned long>(event, "modifierFlags") & (1UL << 20)) return false; // Command: Mac shortcuts stay here
        auto code = send<unsigned short>(event, "keyCode");
        if (code == 53) return action(1), true; // esc -> Back
        if (const auto* it = std::ranges::find(kKeys, code, &std::pair<unsigned short, uint16_t>::first);
            it != std::end(kKeys)) {
            input_.key({1, uint8_t(it->second >> 8U), uint8_t(it->second & 0xFFU)});
            return true;
        }
        std::string text = toString(send(event, "characters"));
        if (text.empty() || std::ranges::any_of(text, [](char c) { return static_cast<unsigned char>(c) < 32; }))
            return true;
        Bytes k{0};
        k.insert(k.end(), text.begin(), text.begin() + ptrdiff_t(std::min<size_t>(text.size(), 255)));
        input_.key(k);
        return true;
    }

    void action(uint8_t a) const {
        if (input_.key) input_.key({2, a});
    }

    void addNavigationButtons() {
        Obj stack = send(cls("NSStackView"), "stackViewWithViews:", send(cls("NSArray"), "array"));
        const std::pair<const char*, const char*> buttons[] = {
            {"chevron.backward", "Back"}, {"circle", "Home"}, {"square.on.square", "Recents"}};
        long tag = 1;
        for (auto [symbol, label] : buttons) {
            Obj image = send(cls("NSImage"), "imageWithSystemSymbolName:accessibilityDescription:", nsString(symbol),
                             nsString(label));
            Obj b = send(cls("NSButton"), "buttonWithImage:target:action:", image, view_.get(), sel("navigate:"));
            send<void>(b, "setTag:", tag++);
            send<void>(b, "setBezelStyle:", 7UL); // textured rounded
            send<void>(b, "setToolTip:", nsString(label));
            send<void>(stack, "addArrangedSubview:", b);
        }
        send<void>(stack, "setFrameSize:", send<CGSize>(stack, "fittingSize")); // zero would hide it
        Obj vc = send(make("NSTitlebarAccessoryViewController"), "autorelease");
        send<void>(vc, "setView:", stack);
        send<void>(vc, "setLayoutAttribute:", 2L); // trailing
        send<void>(window_.get(), "addTitlebarAccessoryViewController:", vc);
    }

    static Class viewClass() {
        static Class c = [] {
            return ClassBuilder("SpanlyTabletView", "NSView")
                .protocol("NSWindowDelegate")
                .method(
                    "acceptsFirstResponder", +[](Obj, SEL) -> BOOL { return YES; }, "c@:")
                .method(
                    "mouseDown:", +[](Obj o, SEL, Obj e) { owner<MacVideoWindow>(o)->pointer(0, e); }, "v@:@")
                .method(
                    "mouseDragged:", +[](Obj o, SEL, Obj e) { owner<MacVideoWindow>(o)->pointer(1, e); }, "v@:@")
                .method(
                    "mouseUp:", +[](Obj o, SEL, Obj e) { owner<MacVideoWindow>(o)->pointer(2, e); }, "v@:@")
                .method(
                    "rightMouseDown:", +[](Obj o, SEL, Obj e) { owner<MacVideoWindow>(o)->pointer(3, e); }, "v@:@")
                .method(
                    "scrollWheel:", +[](Obj o, SEL, Obj e) { owner<MacVideoWindow>(o)->scroll(e); }, "v@:@")
                .method(
                    "keyDown:",
                    +[](Obj o, SEL, Obj e) {
                        if (!owner<MacVideoWindow>(o)->key(e)) sendSuper(o, "keyDown:", e);
                    },
                    "v@:@")
                .method(
                    "navigate:",
                    +[](Obj o, SEL, Obj sender) {
                        owner<MacVideoWindow>(o)->action(uint8_t(send<long>(sender, "tag")));
                    },
                    "v@:@")
                .method(
                    "layout",
                    +[](Obj o, SEL) {
                        sendSuper(o, "layout");
                        auto* w = owner<MacVideoWindow>(o);
                        send<void>(w->display_.get(), "setFrame:", send<CGRect>(o, "bounds"));
                    },
                    "v@:")
                .method(
                    "windowWillClose:",
                    +[](Obj o, SEL, Obj) {
                        auto* w = owner<MacVideoWindow>(o);
                        if (w->input_.closed) w->input_.closed();
                    },
                    "v@:@")
                .build();
        }();
        return c;
    }

    Input input_;
    dispatch_queue_t queue_;
    Ref window_, view_, display_;
    CMVideoFormatDescriptionRef format_ = nullptr; // decode queue only
    int videoW_ = 16, videoH_ = 10;
};

} // namespace

std::unique_ptr<VideoWindow> VideoWindow::create(const std::string& title, Input input) {
    return std::make_unique<MacVideoWindow>(title, std::move(input));
}

} // namespace spanly::platform
