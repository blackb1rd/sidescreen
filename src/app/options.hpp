#pragma once
// Command-line options: they override the menu settings for one run.

#include <optional>
#include <string>

namespace spanly {

struct Options {
    std::optional<std::string> displayName; // stream an existing display instead of a virtual one
    std::optional<bool> hiDPI;              // unset: Retina over USB, standard over adb/Wi-Fi
    std::string codec = "hevc";
    int batteryFps = 30; // while on battery (0 = same as fps)
    bool stats = false;
    std::optional<std::string> position;
    double lingerSeconds = 10;
    int fps = 60;
    std::optional<double> bitrateMbps;
    int maxWidth = 0;
    bool manageAdb = true;
    std::optional<bool> restoreCursor;
    bool usb = true;

    /// Exits on --help or an unknown option.
    static Options parse(int argc, char** argv);
};

} // namespace spanly
