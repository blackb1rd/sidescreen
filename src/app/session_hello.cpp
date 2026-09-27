// A tablet announcing itself (HELLO): make or reshape its display, or just resume the stream.
#include "app/controller.hpp"
#include "app/session.hpp"
#include "core/log.hpp"

namespace spanly {

void TabletSession::hello(const Hello& h) {
    lastHello_ = h;
    ++teardownGeneration_;
    bool hevc = (h.caps & 1U) != 0;
    if (hevc != tabletHEVC_) {
        tabletHEVC_ = hevc;
        if (encoder() && codec() != wantedCodec()) {
            size_ = {0, 0}; // force a SIZE (with the new codec) once the pipeline restarts
            scheduleRestart(0.2);
            return;
        }
    }
    if (c_.opts().displayName || c_.mirroring()) {
        virtual_.reset();
        if (size_.first > 0) {
            followLink();
        } else {
            size_ = {0, 0};
            scheduleRestart(0.1);
        }
        return;
    }
    int tw = h.w, th = h.h; // as the tablet is held: landscape or portrait
    if (virtual_ && virtual_->hiDPI() == wantHiDPI() && virtual_->tabletW() == th && virtual_->tabletH() == tw) {
        // The tablet turned: reshape the same display so its windows stay on it.
        if (virtual_->resize(tw, th)) {
            log("{} rotated: display is now {}x{}", name(), tw, th);
            size_ = {0, 0};
            std::weak_ptr<TabletSession> weak = weak_from_this();
            platform::runAfter(0.5, [weak] {
                auto s = weak.lock();
                if (!s || !s->virtual_) return;
                s->virtual_->selectMode();
                s->scheduleRestart(0.3);
            });
            return;
        }
    }
    if (virtual_ && virtual_->tabletW() == tw && virtual_->tabletH() == th && virtual_->hiDPI() == wantHiDPI()) {
        // A repeated HELLO while the display is still being set up: the pipeline start will send
        // SIZE and a keyframe when it's ready.
        if (size_.first == 0) return;
        followLink(); // maybe a new link (e.g. USB -> Wi-Fi)
        return;
    }
    virtual_.reset();
    size_ = {0, 0};
    virtual_ = platform::VirtualDisplay::create(tw, th, h.dpi, wantHiDPI(), serial());
    if (!virtual_) {
        // Usually the previous display (same serial) hasn't finished going away yet.
        log("could not create virtual display; retrying");
        std::weak_ptr<TabletSession> weak = weak_from_this();
        platform::runAfter(1, [weak] {
            auto s = weak.lock();
            if (s && !s->virtual_ && s->link()->connected() && s->lastHello_) s->hello(*s->lastHello_);
        });
        return;
    }
    log("created virtual display {} for {}, {}x{} ({}, over {})", virtual_->id(), name(), tw, th,
        virtual_->hiDPI() ? "HiDPI" : "standard", Link::kindName(link()->kind()));
    // The OS needs a moment to publish the new display's modes and geometry.
    std::weak_ptr<TabletSession> weak = weak_from_this();
    platform::runAfter(0.5, [weak] {
        auto s = weak.lock();
        if (!s || !s->virtual_) return;
        s->virtual_->selectMode();
        s->virtual_->place(s->c_.position(), s->c_.virtualDisplays());
        s->scheduleRestart(0.5);
    });
}

void TabletSession::recreateDisplay() {
    if (!lastHello_ || !link()->connected()) return;
    std::weak_ptr<TabletSession> weak = weak_from_this();
    stop([weak] {
        // Give the OS a moment to remove the old display before making the new one.
        platform::runAfter(0.5, [weak] {
            if (auto s = weak.lock(); s && s->lastHello_) s->hello(*s->lastHello_);
        });
    });
}

/// Keep the display briefly across reconnects so windows don't jump around; then let go.
void TabletSession::scheduleTeardown() {
    uint64_t gen = ++teardownGeneration_;
    std::weak_ptr<TabletSession> weak = weak_from_this();
    platform::runAfter(c_.opts().lingerSeconds, [weak, gen] {
        auto s = weak.lock();
        if (!s || gen != s->teardownGeneration_ || s->link()->connected()) return;
        s->stop([weak] {
            if (auto s = weak.lock()) {
                log("{} gone; stopped streaming", s->name());
                s->c_.remove(s);
            }
        });
    });
}

} // namespace spanly
