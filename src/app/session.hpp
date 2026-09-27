#pragma once
// One tablet: the connection it is using (the one it last said HELLO on), its virtual display,
// and the capture -> encode -> send pipeline for it. Main thread, except for the capture and
// encoder callbacks.

#include "app/stats.hpp"
#include "core/flow.hpp"
#include "core/link.hpp"
#include "platform/platform.hpp"

#include <atomic>
#include <memory>
#include <mutex>

namespace spanly {

class Controller;

class TabletSession : public std::enable_shared_from_this<TabletSession> {
public:
    TabletSession(std::string id, Controller& controller);
    ~TabletSession();

    const std::string& id() const { return id_; }

    /// The tablet said HELLO on `l`: that is where it listens now.
    void activate(const std::shared_ptr<Link>& l);
    /// Its current connection (Link::none() when it has none).
    std::shared_ptr<Link> link() const;
    bool onUsb() const { return link()->kind() == Link::Kind::Usb; }
    bool onWifi() const { return link()->kind() == Link::Kind::Wifi; }
    bool streaming() const;
    std::string name() const;
    /// Serial number of its virtual display: the OS keeps each tablet's arrangement.
    uint32_t serial() const;

    void hello(const Hello& h);
    void setViewing(bool on);
    void acked(uint32_t frameId);
    /// Make the display again (e.g. Retina switched on), keeping the tablet connected.
    void recreateDisplay();
    /// Restart capture with the current settings (e.g. sound on/off), keeping the display.
    void restartCapture();
    void scheduleRestart(double delay = 1);
    void scheduleTeardown();
    /// Stop streaming and remove the display; `done` runs on the main thread.
    void stop(std::function<void()> done);

    void adaptBitrate();
    void updateCursorVisibility(platform::Point cursor, bool touchActive);
    void setDisplayAwake(bool on);
    void placeDisplay(const std::string& position, const std::vector<platform::DisplayId>& others);

    platform::DisplayId displayId() const { return displayId_; }
    std::optional<platform::DisplayId> virtualDisplayId() const;
    std::pair<int, int> size() const { return size_; }
    platform::Codec codec() const;
    Stats& stats() { return stats_; }

private:
    void startPipeline();
    void followLink();
    void startStream();
    void sendSize();
    void send(Bytes au, bool key, std::optional<Bytes> config, Clock::time_point captured);
    void onCapturedFrame(const platform::Frame& f);
    std::optional<std::tuple<platform::DisplayId, int, int>> findDisplay() const;
    std::pair<int, int> streamSize() const;
    std::shared_ptr<platform::Encoder> makeEncoder(int w, int h, double bitrateMbps);
    std::shared_ptr<platform::Encoder> encoder() const;
    bool wantHiDPI() const;
    double bitrateMbps() const;
    double maxBitrateMbps() const;
    int currentFps() const;
    platform::Codec wantedCodec() const;

    const std::string id_;
    Controller& c_;
    std::unique_ptr<platform::Capture> capture_;
    std::unique_ptr<platform::VirtualDisplay> virtual_;
    FlowControl flow_;
    Stats stats_;

    mutable std::mutex m_; // guards the fields other threads read
    std::weak_ptr<Link> active_;
    std::shared_ptr<platform::Encoder> encoder_;

    std::optional<Hello> lastHello_;
    std::pair<int, int> size_{0, 0};
    std::pair<int, int> pixels_{0, 0}; // the captured display's size in pixels
    platform::DisplayId displayId_ = 0;
    bool tabletHEVC_ = false;
    double activeBitrate_ = 0;
    double currentBitrate_ = 0;
    uint64_t restartGeneration_ = 0;
    uint64_t teardownGeneration_ = 0;
    std::atomic<bool> viewing_{true};
    std::atomic<bool> waitingForKey_{false};
};

} // namespace spanly
