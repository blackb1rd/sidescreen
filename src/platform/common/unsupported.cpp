// What Windows and Linux don't have yet: the computer's sound on the tablet (and muting it), the
// tablet's microphone, the tablet's own screen in a window, and spanly:// links.
#include "platform/platform.hpp"

namespace spanly::platform {

void muteSpeakers() {}
void restoreSpeakers() {}

std::unique_ptr<PcmPlayer> PcmPlayer::create(int, const std::string&) {
    return nullptr;
}

bool microphoneInstalled() {
    return false;
}
void installMicrophone() {}

// NOLINTNEXTLINE(performance-unnecessary-value-param): the interface takes ownership
std::unique_ptr<VideoWindow> VideoWindow::create(const std::string&, Input) {
    return nullptr;
}

void onOpenUrl(std::function<void(const std::string&)>) {} // NOLINT(performance-unnecessary-value-param)

} // namespace spanly::platform
