#pragma once
// The menu bar / tray menu: status, which tablet to use, and settings. First launch: open at
// login, and a guide to the permissions the app needs.

#include "platform/platform.hpp"

#include <set>
#include <string>

namespace spanly {

class Controller;

class Menu {
public:
    explicit Menu(Controller& controller);
    void onboardIfNeeded();

private:
    std::vector<platform::MenuItem> build();
    std::vector<platform::MenuItem> tabletItems();
    void tick();

    Controller& c_;
    std::unique_ptr<platform::Tray> tray_;
    std::set<std::string> offered_; // tablets already offered this session
};

} // namespace spanly
