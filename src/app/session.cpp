#include "app/session.hpp"

#include "app/controller.hpp"
#include "core/log.hpp"

#include <algorithm>
#include <cmath>

namespace spanly {

using platform::Codec;

TabletSession::TabletSession(std::string id, Controller& controller)
    : id_(std::move(id)), c_(controller), capture_(platform::Capture::create()) {
    capture_->onFrame = [this](const platform::Frame& f) { onCapturedFrame(f); };
    capture_->onStop = [this](bool byUser) {
        if (byUser)
            pause();
        else
            scheduleRestart();
    };
    capture_->onAudio = [this](const Bytes& pcm) {
        auto l = link();
        if (!l->connected()) return;
        if (c_.opts().stats) stats_.audio(pcm);
        l->send(Msg::Audio, pcm);
    };
}

TabletSession::~TabletSession() {
    capture_->onFrame = nullptr;
    capture_->onAudio = nullptr;
    capture_->onStop = nullptr;
}

// MARK: Link

void TabletSession::activate(const std::shared_ptr<Link>& l) {
    std::scoped_lock lock(m_);
    active_ = l;
}

std::shared_ptr<Link> TabletSession::link() const {
    std::scoped_lock lock(m_);
    auto l = active_.lock();
    return l && l->connected() ? l : Link::none();
}

std::shared_ptr<platform::Encoder> TabletSession::encoder() const {
    std::scoped_lock lock(m_);
    return encoder_;
}

bool TabletSession::streaming() const {
    return !paused_ && link()->connected() && encoder();
}

void TabletSession::pause() {
    std::weak_ptr<TabletSession> weak = weak_from_this();
    platform::runOnMain([weak] {
        auto s = weak.lock();
        if (!s || s->paused_.exchange(true)) return;
        log("stopped sharing to {}; choose \"Resume Sharing\" in the Spanly menu to share again", s->name());
        s->link()->send(Msg::StreamStop); // the tablet drops the last picture
        Bytes off{0};
        s->link()->send(Msg::Display, off); // and may let its screen sleep
        s->stop([weak] {
            if (auto s = weak.lock()) s->c_.updateSpeakers();
        });
    });
}

void TabletSession::resume() {
    if (!paused_.exchange(false)) return;
    log("sharing to {} again", name());
    if (lastHello_ && link()->connected()) hello(*lastHello_);
}

std::string TabletSession::name() const {
    if (onUsb() && c_.usb()) {
        if (auto t = c_.usb()->tablet()) return t->name;
    }
    if (lastHello_ && !lastHello_->name.empty()) return lastHello_->name;
    return c_.settings().deviceName().value_or("tablet");
}

uint32_t TabletSession::serial() const {
    if (id_ == Controller::kAnonymousTablet) return 1;
    uint32_t hash = 2166136261U; // FNV-1a
    for (unsigned char ch : id_)
        hash = (hash ^ ch) * 16777619U;
    return std::max<uint32_t>(2, hash & 0x7fffffffU);
}

platform::Codec TabletSession::codec() const {
    auto e = encoder();
    return e ? e->codec() : Codec::H264;
}

std::optional<platform::DisplayId> TabletSession::virtualDisplayId() const {
    return virtual_ ? std::optional(virtual_->id()) : std::nullopt;
}

// MARK: Pipeline

std::optional<std::tuple<platform::DisplayId, int, int>> TabletSession::findDisplay() const {
    platform::DisplayId id = 0;
    if (c_.mirroring()) {
        id = platform::mainDisplay();
    } else if (c_.opts().displayName) {
        auto named = platform::displayNamed(*c_.opts().displayName);
        if (!named) return std::nullopt;
        id = *named;
    } else if (virtual_) {
        id = virtual_->id();
    } else {
        return std::nullopt;
    }
    auto px = platform::displayPixels(id);
    if (!px) return std::nullopt;
    return std::tuple{id, px->first, px->second};
}

/// The size to encode at: the display's pixels, within the tablet's decoder limits, and smaller
/// on Wi-Fi (fewer pixels at a Wi-Fi bitrate: smoother, with less lag).
std::pair<int, int> TabletSession::streamSize() const {
    auto [w, h] = pixels_;
    auto fit = [&](int maxW, int maxH) {
        if (maxW <= 0 || maxH <= 0 || (w <= maxW && h <= maxH)) return;
        double scale = std::min(double(maxW) / w, double(maxH) / h);
        w = int(w * scale);
        h = int(h * scale);
    };
    if (c_.opts().maxWidth > 0) fit(c_.opts().maxWidth, 1 << 20);
    if (lastHello_) fit(lastHello_->maxW, lastHello_->maxH); // e.g. 2560x1440 on MediaTek
    if (onWifi()) fit(1280, 1280);
    return {w & ~15, h & ~15}; // whole macroblocks
}

std::shared_ptr<platform::Encoder> TabletSession::makeEncoder(int w, int h, double mbps) {
    int bps = int(mbps * 1'000'000);
    std::shared_ptr<platform::Encoder> enc = platform::Encoder::create(w, h, currentFps(), bps, wantedCodec());
    if (!enc && wantedCodec() != Codec::H264) enc = platform::Encoder::create(w, h, currentFps(), bps, Codec::H264);
    if (!enc) return nullptr;
    activeBitrate_ = currentBitrate_ = mbps;
    std::weak_ptr<TabletSession> weak = weak_from_this();
    enc->onFrame = [weak](Bytes au, bool key, std::optional<Bytes> config, Clock::time_point captured) {
        if (auto s = weak.lock()) s->send(std::move(au), key, std::move(config), captured);
    };
    return enc;
}

void TabletSession::startPipeline() {
    if (paused_) return;
    if (!c_.displayOn()) return; // a sleeping display can't be captured; waking restarts us
    auto found = findDisplay();
    if (!found) {
        if (c_.opts().displayName) {
            log("display \"{}\" not found; retrying", *c_.opts().displayName);
            scheduleRestart(3);
        }
        return;
    }
    auto [id, pw, ph] = *found;
    pixels_ = {pw, ph};
    auto [w, h] = streamSize();
    double mbps = bitrateMbps();
    auto enc = makeEncoder(w, h, mbps);
    if (!enc) return;
    bool sizeChanged = size_ != std::pair{w, h};
    {
        std::scoped_lock lock(m_);
        encoder_ = enc;
    }
    displayId_ = id;
    size_ = {w, h};
    // Only the first tablet plays the Mac's sound: two would echo.
    bool audio = c_.settings().sound() != Sound::Mac && c_.primary().get() == this;
    int fps = currentFps();
    std::weak_ptr<TabletSession> weak = weak_from_this();
    capture_->start(id, w, h, fps, audio, [weak, id, w, h, fps, mbps, sizeChanged, enc](const std::string& error) {
        platform::runOnMain([weak, id, w, h, fps, mbps, sizeChanged, enc, error] {
            auto s = weak.lock();
            if (!s) return;
            if (!error.empty()) {
                log("capture failed: {}{}", error,
                    platform::screenRecordingAllowed() ? "" : " (allow Spanly in Screen Recording settings)");
                s->scheduleRestart(3);
                return;
            }
            log("capturing display {} at {}x{} @ {} fps{}, {}, {} Mbit/s", id, w, h, fps,
                s->c_.onBattery() ? " (on battery)" : "", enc->codec() == Codec::Hevc ? "hevc" : "h264", mbps);
            if (s->link()->connected()) {
                s->flow_.setMaxInFlight(s->onWifi() ? 6 : 3);
                // Streaming again (e.g. after "Resume Sharing"): the tablet keeps its screen on.
                Bytes awake{uint8_t(s->c_.displayOn() ? 1 : 0)};
                s->link()->send(Msg::Display, awake);
                if (sizeChanged) s->sendSize();
                enc->requestKeyframe();
            }
            s->c_.updateSpeakers();
            s->c_.updateMicrophone();
        });
    });
}

void TabletSession::scheduleRestart(double delay) {
    std::weak_ptr<TabletSession> weak = weak_from_this();
    platform::runOnMain([weak, delay] {
        auto s = weak.lock();
        if (!s) return;
        uint64_t gen = ++s->restartGeneration_;
        platform::runAfter(delay, [weak, gen] {
            auto s = weak.lock();
            if (!s || gen != s->restartGeneration_) return; // replaced by a later restart
            s->capture_->stop([weak] {
                platform::runOnMain([weak] {
                    if (auto s = weak.lock()) s->startPipeline();
                });
            });
        });
    });
}

void TabletSession::restartCapture() {
    if (!encoder()) return;
    size_ = {0, 0}; // re-send SIZE and a keyframe once capture restarts
    scheduleRestart(0.1);
}

/// The tablet moved to another link (e.g. the cable came out): new bitrate, and a new stream
/// size if that link wants one. Capture keeps running and is resized in place.
void TabletSession::followLink() {
    auto [w, h] = streamSize();
    if (size_ != std::pair{w, h} && pixels_.first > 0) {
        if (auto enc = makeEncoder(w, h, bitrateMbps())) {
            {
                std::scoped_lock lock(m_);
                encoder_ = enc;
            }
            size_ = {w, h};
            capture_->resize(w, h);
            log("{}: streaming at {}x{}, {} Mbit/s over {}", name(), w, h, bitrateMbps(),
                Link::kindName(link()->kind()));
        }
    } else if (activeBitrate_ != bitrateMbps()) {
        activeBitrate_ = currentBitrate_ = bitrateMbps();
        if (auto e = encoder()) e->setBitrate(int(bitrateMbps() * 1'000'000));
    }
    startStream();
}

void TabletSession::stop(std::function<void()> done) {
    ++restartGeneration_;
    ++teardownGeneration_;
    std::weak_ptr<TabletSession> weak = weak_from_this();
    capture_->stop([weak, done = std::move(done)] {
        platform::runOnMain([weak, done] {
            if (auto s = weak.lock()) {
                {
                    std::scoped_lock lock(s->m_);
                    s->encoder_.reset();
                }
                s->virtual_.reset();
                s->size_ = {0, 0};
            }
            if (done) done();
        });
    });
}

void TabletSession::setViewing(bool on) {
    if (on == viewing_.exchange(on)) return;
    if (on && encoder()) startStream();
}

void TabletSession::requestKeyframe() {
    if (auto e = encoder()) e->requestKeyframe();
}

void TabletSession::acked(uint32_t frameId) {
    if (auto ms = flow_.acked(frameId)) stats_.latency(*ms);
}

void TabletSession::startStream() {
    c_.updateSpeakers();
    c_.updateMicrophone();
    flow_.reset(onWifi() ? 6 : 3);
    sendSize();
    Bytes on{uint8_t(c_.displayOn() ? 1 : 0)};
    link()->send(Msg::Display, on);
    waitingForKey_ = true;
    if (auto e = encoder()) e->requestKeyframe();
    capture_->resendLast();
}

void TabletSession::sendSize() const {
    Bytes p;
    putU32(p, uint32_t(size_.first));
    putU32(p, uint32_t(size_.second));
    putU32(p, uint32_t(codec()));
    link()->send(Msg::Size, p);
}

/// Capture thread.
void TabletSession::onCapturedFrame(const platform::Frame& f) {
    // Nothing to encode while the tablet's app is in the background.
    if (!viewing_ || !link()->connected()) return;
    // Link backed up: skip this capture rather than encode it. Skipping before the encoder keeps
    // the reference chain intact, so no keyframe is needed to recover.
    if (flow_.tooManyInFlight()) {
        stats_.drop();
        return;
    }
    if (auto e = encoder()) e->encode(f);
}

/// Encoder thread.
void TabletSession::send(Bytes au, bool key, std::optional<Bytes> config, Clock::time_point captured) {
    auto l = link();
    if (!l->connected()) return;
    if (!key && waitingForKey_) return;
    waitingForKey_ = false;
    if (config) l->send(Msg::Config, *config);
    uint32_t frameId = flow_.registerFrame(captured);
    Bytes p;
    p.reserve(au.size() + 5);
    p.push_back(key ? 1 : 0);
    putU32(p, frameId);
    p.insert(p.end(), au.begin(), au.end());
    if (c_.opts().stats) stats_.frame(p.size());
    l->send(Msg::Frame, p);
}

// MARK: Tuning

bool TabletSession::wantHiDPI() const {
    if (c_.opts().hiDPI) return *c_.opts().hiDPI;
    switch (c_.settings().quality()) {
    case Quality::Retina: return true;
    case Quality::Standard: return false;
    case Quality::Auto:
        // Keep an existing display when the link changes (rebuilding it would scatter its windows).
        if (virtual_) return virtual_->hiDPI();
        return onUsb(); // Retina needs the bandwidth of the raw USB link
    }
    return false;
}

/// Starting bitrate. USB carries ~55 Mbit/s before frames back up (Redmi Pad 2, full-screen noise).
double TabletSession::bitrateMbps() const {
    if (c_.opts().bitrateMbps) return *c_.opts().bitrateMbps;
    return onUsb() ? 40 : onWifi() ? 12 : 6;
}

/// Wi-Fi varies from a crowded 2.4 GHz channel to fast 5 GHz: it starts low and finds its level.
double TabletSession::maxBitrateMbps() const {
    if (c_.opts().bitrateMbps) return *c_.opts().bitrateMbps;
    return onWifi() ? 30 : bitrateMbps();
}

int TabletSession::currentFps() const {
    const auto& o = c_.opts();
    return c_.onBattery() && o.batteryFps > 0 ? std::min(o.batteryFps, o.fps) : o.fps;
}

platform::Codec TabletSession::wantedCodec() const {
    return c_.opts().codec == "hevc" && tabletHEVC_ ? Codec::Hevc : Codec::H264;
}

void TabletSession::adaptBitrate() {
    auto e = encoder();
    if (!e || !link()->connected()) return;
    auto target = spanly::adaptBitrate(currentBitrate_, maxBitrateMbps(), flow_.takeWindow());
    if (!target) return;
    currentBitrate_ = *target;
    e->setBitrate(int(*target * 1'000'000));
    if (c_.opts().stats) log("{}: bitrate -> {:.1f} Mbit/s", name(), *target);
}

/// Show the cursor on the tablet only when the computer's own mouse put it there, never because
/// a touch borrowed it (that would leave a stray pointer in the picture).
void TabletSession::updateCursorVisibility(platform::Point cursor, bool touchActive) {
    if (!encoder()) return;
    capture_->setShowsCursor(platform::displayBounds(displayId_).contains(cursor) && !touchActive);
}

void TabletSession::placeDisplay(const std::string& position, const std::vector<platform::DisplayId>& others) {
    if (virtual_) virtual_->place(position, others);
}

void TabletSession::setDisplayAwake(bool on) {
    if (paused_) return; // the tablet already knows it may sleep
    Bytes p{uint8_t(on ? 1 : 0)};
    link()->send(Msg::Display, p);
    if (on && encoder()) scheduleRestart(0.5);
}

} // namespace spanly
