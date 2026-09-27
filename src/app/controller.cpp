#include "app/controller.hpp"

#include "core/log.hpp"

#include <algorithm>
#include <format>

namespace spanly {

Controller::Controller(Options opts) : opts_(std::move(opts)), pointer_(platform::Pointer::create()) {
    if (opts_.usb) usb_ = UsbLink::create([this] { return chosenSerial(); });
    adb_ = std::make_unique<TcpListener>(TcpListener::kAdbPort, Link::Kind::Adb);
    wifi_ = std::make_unique<TcpListener>(TcpListener::kWifiPort, Link::Kind::Wifi,
                                          [this] { return settings_.wifiSecret(); });
    beacon_ = std::make_unique<Beacon>(platform::computerName(), TcpListener::kWifiPort);
    if (opts_.manageAdb) {
        adbTool_ = Adb::find(TcpListener::kAdbPort);
        if (!adbTool_) log("adb not found (set ADB=/path/to/adb); the USB accessory and Wi-Fi links work without it");
    }
}

Controller::~Controller() = default;

void Controller::start() {
    if (!platform::accessibilityAllowed())
        log("touch input disabled until Spanly has Accessibility permission "
            "(System Settings > Privacy & Security > Accessibility)");
    pointer_->restoreCursor = opts_.restoreCursor.value_or(settings_.restoreCursor());
    if (usb_) wire(usb_);
    adb_->onConnection = [this](const std::shared_ptr<TcpLink>& l) { wire(l); };
    adb_->setEnabled(true);
    wifi_->onConnection = [this](const std::shared_ptr<TcpLink>& l) { wire(l); };
    setWifi(settings_.wifi());

    every(0.1, [this] {
        auto here = platform::cursorLocation();
        for (auto& s : sessions_)
            s->updateCursorVisibility(here, pointer_->touchActive());
    });
    every(1, [this] {
        for (auto& s : sessions_)
            s->adaptBitrate();
    });
    if (opts_.stats) {
        every(5, [this] {
            for (auto& s : sessions_) {
                if (auto line = s->stats().report(5)) log("{}: {}", s->name(), *line);
            }
        });
    }
    platform::watchSystem({
        .displayPower = [this](bool awake) { setDisplayAwake(awake); },
        .screensChanged =
            [this] {
                for (auto& s : sessions_) {
                    if (opts_.displayName || s->virtualDisplayId() || (mirroring() && s->streaming()))
                        s->scheduleRestart();
                }
            },
        .willQuit = [] { platform::restoreSpeakers(); },
    });
    onBattery_ = platform::onBattery();
    every(5, [this] { checkPower(); });
    if (adbTool_) adbTool_->startWatching();
    log("listening on 127.0.0.1:{}; a virtual display appears when a tablet connects", TcpListener::kAdbPort);
}

void Controller::every(double seconds, std::function<void()> fn) {
    auto tick = std::make_shared<std::function<void()>>();
    *tick = [seconds, fn = std::move(fn), weak = std::weak_ptr(tick)] {
        fn();
        if (auto t = weak.lock()) platform::runAfter(seconds, [t] { (*t)(); });
    };
    static std::vector<std::shared_ptr<std::function<void()>>> timers; // live as long as the app
    timers.push_back(tick);
    platform::runAfter(seconds, [tick] { (*tick)(); });
}

std::string Controller::chosenSerial() const {
    if (auto s = settings_.deviceSerial()) return *s;
    return adbTool_ ? adbTool_->serial().value_or("") : "";
}

std::vector<UsbTablet> Controller::tablets() const {
    return usb_ ? usb_->tablets() : std::vector<UsbTablet>{};
}

std::vector<std::pair<std::string, std::string>> Controller::streamStatus() const {
    std::vector<std::pair<std::string, std::string>> out;
    for (const auto& s : sessions_) {
        auto [w, h] = s->size();
        if (!s->streaming() || w == 0) continue;
        const char* via = s->onUsb() ? "USB" : s->onWifi() ? "Wi-Fi" : "adb (slower)";
        out.emplace_back(s->name(), std::format("{}×{} · {} · {}", w, h,
                                                s->codec() == platform::Codec::Hevc ? "HEVC" : "H.264", via));
    }
    return out;
}

void Controller::applySettings(bool recreateDisplay) {
    pointer_->restoreCursor = opts_.restoreCursor.value_or(settings_.restoreCursor());
    if (recreateDisplay) {
        for (auto& s : sessions_)
            s->recreateDisplay();
        return;
    }
    for (auto& s : sessions_)
        s->placeDisplay(position(), virtualDisplays());
}

void Controller::restartCapture() {
    for (auto& s : sessions_)
        s->restartCapture();
}

void Controller::checkPower() {
    bool now = platform::onBattery();
    if (now == onBattery_) return;
    onBattery_ = now;
    if (opts_.batteryFps <= 0 || opts_.batteryFps >= opts_.fps) return;
    log("{}",
        now ? std::format("on battery -> {} fps", opts_.batteryFps) : std::format("on power -> {} fps", opts_.fps));
    for (auto& s : sessions_) {
        if (s->streaming()) s->scheduleRestart(0.2);
    }
}

void Controller::chooseTablet(const UsbTablet& t) {
    settings_.setDevice(t.serial, t.name);
    log("using {} ({}) as the second screen", t.name, t.serial);
}

// MARK: Links

/// Route a connection's messages to the tablet it belongs to. Callbacks arrive on the link's thread.
void Controller::wire(const std::shared_ptr<Link>& l) {
    std::weak_ptr<Link> weak = l;
    l->onHello = [this, weak](const Hello& h) {
        platform::runOnMain([this, weak, h] {
            if (auto l = weak.lock()) hello(h, l);
        });
    };
    l->onDisconnect = [this, weak] {
        platform::runOnMain([this, weak] {
            if (auto l = weak.lock()) closed(l);
        });
    };
    l->onAck = [weak](uint32_t frameId) {
        auto l = weak.lock();
        if (auto s = l ? l->session() : nullptr) s->acked(frameId);
    };
    // Input goes to the display of the tablet it came from.
    auto input = [this, weak](std::function<void(platform::Pointer&)> event) {
        auto l = weak.lock();
        auto s = l ? l->session() : nullptr;
        if (!s) return;
        platform::runOnMain([this, s, event = std::move(event)] {
            if (!platform::accessibilityAllowed()) return;
            pointer_->display = s->displayId();
            event(*pointer_);
        });
    };
    l->onTouch = [input](uint8_t a, float x, float y) { input([=](platform::Pointer& p) { p.touch(a, x, y); }); };
    l->onScroll = [input](uint8_t kind, uint8_t phase, bool last, float x, float y, float dx, float dy) {
        input([=](platform::Pointer& p) { p.scroll(kind, phase, last, x, y, dx, dy); });
    };
    l->onPen = [input](uint8_t a, uint8_t b, float x, float y, float pressure) {
        input([=](platform::Pointer& p) { p.pen(a, b, x, y, pressure); });
    };
    l->onZoom = [input](int8_t dir, float x, float y) { input([=](platform::Pointer& p) { p.zoom(dir, x, y); }); };
    l->onMicAudio = [this, weak](const Bytes& pcm) {
        auto l = weak.lock();
        auto s = l ? l->session() : nullptr;
        std::scoped_lock lock(micLock_);
        if (s && s == micSession_.lock() && micPlayer_) micPlayer_->play(pcm);
    };
    l->onViewing = [weak](bool viewing) {
        platform::runOnMain([weak, viewing] {
            auto l = weak.lock();
            if (auto s = l ? l->session() : nullptr) s->setViewing(viewing);
        });
    };
}

void Controller::hello(const Hello& h, const std::shared_ptr<Link>& l) {
    std::string tabletId = h.id.empty() ? kAnonymousTablet : h.id;
    auto it = std::ranges::find_if(sessions_, [&](auto& s) { return s->id() == tabletId; });
    std::shared_ptr<TabletSession> s;
    if (it != sessions_.end()) {
        s = *it;
    } else {
        s = std::make_shared<TabletSession>(tabletId, *this);
        sessions_.push_back(s);
    }
    l->setSession(s);
    s->activate(l);
    if (l == usb_) rememberTablet();
    s->hello(h);
}

void Controller::closed(const std::shared_ptr<Link>& l) {
    auto s = l->session();
    if (!s) return;
    l->setSession(nullptr);             // a reconnect binds again with its HELLO
    if (s->link()->connected()) return; // already moved to another link
    s->setViewing(true);
    updateSpeakers();
    updateMicrophone();
    s->scheduleTeardown();
}

void Controller::remove(const std::shared_ptr<TabletSession>& s) {
    std::erase(sessions_, s);
    if (settings_.sound() != Sound::Mac) { // the next tablet now plays the sound
        if (auto p = primary()) p->restartCapture();
    }
    updateSpeakers();
    updateMicrophone();
}

std::shared_ptr<TabletSession> Controller::primary() const {
    for (auto& s : sessions_) {
        if (s->link()->connected()) return s;
    }
    return sessions_.empty() ? nullptr : sessions_.front();
}

std::vector<platform::DisplayId> Controller::virtualDisplays() const {
    std::vector<platform::DisplayId> ids;
    for (auto& s : sessions_) {
        if (auto id = s->virtualDisplayId()) ids.push_back(*id);
    }
    return ids;
}

/// Plugged in = trusted: hand over the Wi-Fi secret so the tablet can connect wirelessly later.
void Controller::rememberTablet() {
    auto t = usb_ ? usb_->tablet() : std::nullopt;
    if (!t || t->serial.empty()) return;
    Bytes pair = settings_.wifiSecret();
    std::string name = platform::computerName().substr(0, 200);
    pair.insert(pair.end(), name.begin(), name.end());
    usb_->send(Msg::Pair, pair);
    if (settings_.deviceSerial() != t->serial || settings_.deviceName() != t->name)
        settings_.setDevice(t->serial, t->name);
}

void Controller::setWifi(bool enabled) {
    settings_.setWifi(enabled);
    wifi_->setEnabled(enabled);
    if (enabled) {
        beacon_->start(); // for tablets Bonjour doesn't reach (e.g. on the tablet's hotspot)
        log("accepting paired tablets over Wi-Fi on port {}", TcpListener::kWifiPort);
    } else {
        beacon_->stop();
    }
}

/// New Wi-Fi secret: every paired tablet must be plugged in again before using Wi-Fi.
void Controller::forgetPairedTablets() {
    settings_.resetWifiSecret();
    wifi_->setEnabled(false);
    wifi_->setEnabled(settings_.wifi());
    log("Wi-Fi pairing reset");
}

void Controller::setDisplayAwake(bool on) {
    if (on == displayOn_) return;
    displayOn_ = on;
    log("{}", on ? "display awake -> waking tablets" : "display asleep -> sleeping tablets");
    for (auto& s : sessions_)
        s->setDisplayAwake(on);
    if (adbTool_) on ? adbTool_->wakeTablet() : adbTool_->sleepTablet();
}

/// "Tablet Only" sound mutes the computer while it streams to a tablet.
void Controller::updateSpeakers() {
    bool streaming = std::ranges::any_of(sessions_, [](auto& s) { return s->streaming(); });
    if (settings_.sound() == Sound::Tablet && streaming) {
        platform::muteSpeakers();
    } else {
        platform::restoreSpeakers();
    }
}

/// The first tablet's microphone as "Spanly Microphone", to match the setting and the connections.
void Controller::updateMicrophone() {
    std::shared_ptr<TabletSession> target;
    if (settings_.tabletMicrophone() && platform::microphoneInstalled()) {
        if (auto p = primary(); p && p->link()->connected()) target = p;
    }
    std::scoped_lock l(micLock_);
    auto current = micSession_.lock();
    if (target == current && (target != nullptr) == (micPlayer_ != nullptr)) return;
    if (current) current->link()->send(Msg::MicStop);
    micSession_.reset();
    micPlayer_.reset();
    if (!target) return;
    micPlayer_ = platform::PcmPlayer::create(1, platform::kMicrophoneUid);
    if (!micPlayer_) {
        log("could not open Spanly Microphone");
        return;
    }
    micSession_ = target;
    target->link()->send(Msg::MicStart);
    log("using {}'s microphone as Spanly Microphone", target->name());
}

} // namespace spanly
