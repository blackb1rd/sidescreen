// Spanly: use a tablet or phone as a second screen for this computer.
#include "app/controller.hpp"
#include "app/menu.hpp"
#include "core/log.hpp"
#include "platform/platform.hpp"

int main(int argc, char** argv) {
    using namespace spanly;
    int exitCode = 0;
    std::optional<Options> opts = Options::parse(argc, argv, exitCode);
    if (!opts) return exitCode;
    setLogFile(platform::logFilePath());
    platform::restoreSpeakers(); // in case an earlier run stopped while muting them
    auto controller = std::make_unique<Controller>(std::move(*opts));
    std::unique_ptr<Menu> menu;
    return platform::runApp([&] {
        controller->start();
        platform::onOpenUrl([&](const std::string& url) { controller->openUrl(url); });
        menu = std::make_unique<Menu>(*controller);
        menu->onboardIfNeeded();
    });
}
