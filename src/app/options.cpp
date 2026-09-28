#include "app/options.hpp"

#include "core/log.hpp"

#include <cstdio>
#include <cstdlib>
#include <string_view>

namespace spanly {

namespace {

constexpr const char* kUsage =
    R"(usage: spanly [--hidpi | --standard] [--codec hevc|h264] [--position right|left|above|below|keep]
              [--display NAME] [--fps N] [--battery-fps N] [--bitrate MBPS] [--max-width PX] [--stats]
              [--no-usb] [--no-udp] [--no-adb] [--no-restore-cursor]
Options override the menu settings for this run only.
  --hidpi        always use a Retina virtual display (Automatic: Retina over USB)
  --standard     always use a non-Retina display at half the tablet's resolution
  --codec        hevc (default; h264 if the tablet can't decode HEVC) or h264
  --position     right|left|above|below next to the main display, or keep
  --display      stream an existing display with this name instead of creating a virtual one
  --fps          capture frame rate (default 60)
  --battery-fps  frame rate on battery (default 30; 0 = same as --fps)
  --bitrate      video bitrate in Mbit/s (default 40 over USB, 12 over Wi-Fi, 6 over adb)
  --stats        log frame rate, latency and bitrate every 5 seconds
  --max-width    downscale if the display is wider than this many pixels
  --no-usb       don't use USB accessory mode
  --no-udp       keep Wi-Fi video on TCP (by default it goes over UDP when the tablet can take it)
  --no-adb       don't manage adb (reverse port, app launch, tablet sleep/wake)
  --no-restore-cursor  leave the cursor on the tablet after a touch
)";

double number(const char* s) {
    return s ? std::strtod(s, nullptr) : 0;
}

} // namespace

Options Options::parse(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        std::string_view a = argv[i];
        const char* next = i + 1 < argc ? argv[i + 1] : nullptr;
        auto take = [&] {
            ++i;
            return next;
        };
        if (a == "--display" && next)
            o.displayName = take();
        else if (a == "--hidpi")
            o.hiDPI = true;
        else if (a == "--standard")
            o.hiDPI = false;
        else if (a == "--codec" && next)
            o.codec = take();
        else if (a == "--battery-fps" && next)
            o.batteryFps = int(number(take()));
        else if (a == "--stats")
            o.stats = true;
        else if (a == "--position" && next)
            o.position = take();
        else if (a == "--fps" && next)
            o.fps = int(number(take()));
        else if (a == "--bitrate" && next)
            o.bitrateMbps = number(take());
        else if (a == "--max-width" && next)
            o.maxWidth = int(number(take()));
        else if (a == "--no-adb")
            o.manageAdb = false;
        else if (a == "--no-restore-cursor")
            o.restoreCursor = false;
        else if (a == "--no-usb")
            o.usb = false;
        else if (a == "--no-udp")
            o.udp = false;
        else if (a == "-h" || a == "--help") {
            std::fputs(kUsage, stdout);
            std::exit(0);
        } else if (a.starts_with("-psn_")) { // macOS may pass a process serial number
            continue;
        } else {
            log("unknown option {}", a);
            std::exit(2);
        }
    }
    return o;
}

} // namespace spanly
