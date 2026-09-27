// Spanly: use a tablet or phone as a second screen for this computer.
#include "app/controller.hpp"
#include "core/log.hpp"
#include "platform/platform.hpp"

int main(int argc, char** argv) {
    using namespace spanly;
    Options opts = Options::parse(argc, argv);
    setLogFile(platform::logFilePath());
    auto controller = std::make_unique<Controller>(std::move(opts));
    return platform::runApp([&] { controller->start(); });
}
