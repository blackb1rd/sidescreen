#pragma once
// Owns the listeners (raw USB, adb, Wi-Fi) and one TabletSession per connected tablet.
// Connections are matched to tablets by the ID in their HELLO, so a tablet moving from USB to
// Wi-Fi keeps its display, and two tablets (e.g. one on USB, one on Wi-Fi) get one each.
// Main thread only, except where noted.

#include "app/adb.hpp"
#include "app/options.hpp"
#include "app/session.hpp"
#include "app/settings.hpp"
#include "core/beacon.hpp"
#include "core/listeners.hpp"
#include "core/usb.hpp"

#include <memory>
#include <vector>

namespace spanly {

class Controller {
public:
    /// Tablets whose app sends no ID (older versions) share this one.
    static constexpr const char* kAnonymousTablet = "anonymous";

    explicit Controller(Options opts);
    ~Controller();
    void start();

    const Options& opts() const { return opts_; }
    Settings& settings() { return settings_; }
    platform::Pointer& pointer() { return *pointer_; }
    const std::vector<std::shared_ptr<TabletSession>>& sessions() const { return sessions_; }

    /// The tablet sound, microphone and "Show Tablet Screen" go to: the first one connected.
    std::shared_ptr<TabletSession> primary() const;
    std::string position() const { return opts_.position.value_or(settings_.position()); }
    /// Mirror mode shows the main screen instead of a separate virtual display.
    bool mirroring() const { return !opts_.displayName && settings_.mode() == Mode::Mirror; }
    bool onBattery() const { return onBattery_; }
    bool displayOn() const { return displayOn_; }
    std::vector<platform::DisplayId> virtualDisplays() const;

    void remove(const std::shared_ptr<TabletSession>& s);
    void updateSpeakers();
    void updateMicrophone();
    void setDisplayAwake(bool on);

    std::shared_ptr<UsbLink> usb() const { return usb_; }
    void setWifi(bool enabled);
    void forgetPairedTablets();
    void chooseTablet(const UsbTablet& t);
    /// The tablet chosen in the menu; with nothing chosen yet, the one adb sees (if any).
    std::string chosenSerial() const;
    std::vector<UsbTablet> tablets() const;
    /// Each streaming tablet's name and a short description of its stream.
    std::vector<std::pair<std::string, std::string>> streamStatus() const;
    /// Re-apply menu settings to running streams.
    void applySettings(bool recreateDisplay);
    void restartCapture();

private:
    void wire(const std::shared_ptr<Link>& l);
    void hello(const Hello& h, const std::shared_ptr<Link>& l);
    void closed(const std::shared_ptr<Link>& l);
    void rememberTablet();
    void every(double seconds, std::function<void()> fn);
    void checkPower();

    Options opts_;
    Settings settings_;
    std::unique_ptr<platform::Pointer> pointer_;
    std::shared_ptr<UsbLink> usb_;
    std::unique_ptr<TcpListener> adb_;
    std::unique_ptr<TcpListener> wifi_;
    std::unique_ptr<Beacon> beacon_;
    std::unique_ptr<Adb> adbTool_;
    std::vector<std::shared_ptr<TabletSession>> sessions_;
    bool onBattery_ = false;
    bool displayOn_ = true;
};

} // namespace spanly
