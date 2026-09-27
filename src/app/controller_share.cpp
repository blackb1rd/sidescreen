// Showing the tablet's own screen in a window, and controlling the tablet from there.
#include "app/controller.hpp"
#include "core/log.hpp"

namespace spanly {

void Controller::showTabletScreen() {
    auto s = primary();
    if (!s || !s->link()->connected()) return;
    if (window_) return window_->show();
    std::weak_ptr<TabletSession> weak = s;
    auto send = [weak](Msg type, const Bytes& payload) {
        if (auto s = weak.lock()) s->link()->send(type, payload);
    };
    platform::VideoWindow::Input input{
        .pointer =
            [send](uint8_t action, float x, float y) {
                Bytes p{action};
                putU32(p, std::bit_cast<uint32_t>(x));
                putU32(p, std::bit_cast<uint32_t>(y));
                send(Msg::RemotePointer, p);
            },
        .scroll =
            [send](float x, float y, float dx, float dy) {
                Bytes p;
                for (float v : {x, y, dx, dy})
                    putU32(p, std::bit_cast<uint32_t>(v));
                send(Msg::RemoteScroll, p);
            },
        .key = [send](const Bytes& k) { send(Msg::RemoteKey, k); },
        .closed =
            [this, send] {
                send(Msg::ShareStop, {});
                platform::runOnMain([this] { hideTabletScreen(); });
            },
    };
    window_ = platform::VideoWindow::create(s->name(), std::move(input));
    {
        std::scoped_lock l(shareLock_);
        shareSession_ = s;
        shareSound_.reset();
    }
    s->link()->send(Msg::ShareStart);
    log("asked the tablet to share its screen");
}

void Controller::hideTabletScreen() {
    if (!window_) return;
    {
        std::scoped_lock l(shareLock_);
        if (auto s = shareSession_.lock()) s->link()->send(Msg::ShareStop);
        shareSession_.reset();
        shareSound_.reset();
    }
    auto w = std::move(window_); // closing calls `closed` again; window_ is already empty then
    w.reset();
}

/// spanly://show-tablet and spanly://hide-tablet (e.g. from Shortcuts).
void Controller::openUrl(const std::string& url) {
    if (url.ends_with("show-tablet")) showTabletScreen();
    if (url.ends_with("hide-tablet")) hideTabletScreen();
}

/// Only the tablet being shown reaches the window. Link threads.
void Controller::wireShare(const std::shared_ptr<Link>& l) {
    std::weak_ptr<Link> weak = l;
    auto shown = [this, weak] {
        auto l = weak.lock();
        auto s = l ? l->session() : nullptr;
        std::scoped_lock lock(shareLock_);
        return s && s == shareSession_.lock();
    };
    l->onShareSize = [this, shown](int w, int h) {
        if (!shown()) return;
        platform::runOnMain([this, w, h] {
            if (!window_) return;
            window_->setVideoSize(w, h);
            window_->show();
        });
    };
    // The window decodes on its own queue; it outlives these calls (closed on the main thread
    // only after the tablet stops sending).
    l->onShareConfig = [this, shown](const Bytes& d) {
        if (shown())
            platform::runOnMain([this, d] {
                if (window_) window_->config(d);
            });
    };
    l->onShareFrame = [this, shown](const Bytes& d) {
        if (shown())
            platform::runOnMain([this, d] {
                if (window_) window_->frame(d);
            });
    };
    l->onShareAudio = [this, shown](const Bytes& pcm) {
        if (!shown()) return;
        std::scoped_lock lock(shareLock_);
        if (!shareSound_) shareSound_ = platform::PcmPlayer::create(2);
        if (shareSound_) shareSound_->play(pcm);
    };
    l->onShareStatus = [this, shown](uint8_t state, bool control) {
        if (!shown()) return;
        platform::runOnMain([this, state, control] {
            if (!window_) return;
            if (state == 1) return window_->setControlAvailable(control);
            if (state == 2) log("the tablet declined to share its screen");
            hideTabletScreen();
        });
    };
}

} // namespace spanly
