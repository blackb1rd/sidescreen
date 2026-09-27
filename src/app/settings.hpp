#pragma once
// Settings chosen in the menu, persisted in the platform store. Command-line options (Options)
// override them for one run without being saved.

#include "core/protocol.hpp"

#include <optional>
#include <string>

namespace spanly {

enum class Mode { Extend, Mirror };
enum class Quality { Auto, Retina, Standard };
enum class Sound { Mac, Both, Tablet };

class Settings {
public:
    Mode mode() const;
    void setMode(Mode m);
    Quality quality() const;
    void setQuality(Quality q);
    /// right, left, above, below or keep
    std::string position() const;
    void setPosition(const std::string& p);
    /// Accept paired tablets over Wi-Fi (on unless turned off).
    bool wifi() const;
    void setWifi(bool on);
    /// Secret handed to tablets over USB so they can connect over Wi-Fi.
    Bytes wifiSecret();
    Bytes resetWifiSecret();
    Sound sound() const;
    void setSound(Sound s);
    bool tabletMicrophone() const;
    void setTabletMicrophone(bool on);
    bool restoreCursor() const;
    void setRestoreCursor(bool on);
    /// USB serial number of the tablet to use; other devices are never touched.
    std::optional<std::string> deviceSerial() const;
    std::optional<std::string> deviceName() const;
    void setDevice(const std::string& serial, const std::string& name);
};

} // namespace spanly
