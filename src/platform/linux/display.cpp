// Displays on Linux come from the desktop portal: the virtual monitor (extend) or the main one
// (mirror), each a PipeWire stream. Display ids are PipeWire node ids; 0 is "the main screen",
// opened on first use.
#include "core/log.hpp"
#include "platform/linux/portal.hpp"
#include "platform/platform.hpp"

#include <mutex>

namespace spanly::platform {

using portal::PortalSession;

namespace {

std::unique_ptr<PortalSession> gMain; // the main screen's session (mirror mode), main thread

class PortalDisplay : public VirtualDisplay {
public:
    PortalDisplay(std::unique_ptr<PortalSession> s, int w, int h) : s_(std::move(s)), w_(w), h_(h) {}
    DisplayId id() const override { return s_->node(); }
    int tabletW() const override { return w_; }
    int tabletH() const override { return h_; }
    bool hiDPI() const override { return false; }
    /// The virtual monitor takes the size the capture asks for (see capture.cpp).
    bool resize(int w, int h) override {
        w_ = w;
        h_ = h;
        return true;
    }
    void selectMode() override {}
    void place(const std::string&, const std::vector<DisplayId>&) override {} // the desktop's settings decide

private:
    std::unique_ptr<PortalSession> s_;
    int w_, h_;
};

} // namespace

std::unique_ptr<VirtualDisplay> VirtualDisplay::create(int w, int h, int, bool, uint32_t) {
    auto s = PortalSession::open(true);
    if (!s) return nullptr;
    return std::make_unique<PortalDisplay>(std::move(s), w, h);
}

DisplayId mainDisplay() {
    if (!gMain) gMain = PortalSession::open(false);
    return gMain ? gMain->node() : 0;
}

std::optional<std::pair<int, int>> displayPixels(DisplayId id) {
    if (auto* s = portal::sessionFor(id)) return std::pair{s->width(), s->height()};
    return std::nullopt;
}

/// Pointer positions go to the stream itself, so each display is its own coordinate space.
Rect displayBounds(DisplayId id) {
    auto px = displayPixels(id);
    return px ? Rect{0, 0, double(px->first), double(px->second)} : Rect{};
}

std::optional<DisplayId> displayNamed(const std::string&) {
    return std::nullopt; // the portal chooses (in mirror mode, the user does)
}

Point cursorLocation() {
    return {-1, -1}; // unknown under Wayland; the cursor is drawn into the stream
}

} // namespace spanly::platform
