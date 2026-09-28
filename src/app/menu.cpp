#include "app/menu.hpp"

#include "app/controller.hpp"
#include "platform/platform.hpp"
#include "spanly_version.hpp"

#include <format>

namespace spanly {

using platform::MenuItem;

namespace {

std::vector<MenuItem> choices(const std::vector<std::pair<std::string, std::string>>& options,
                              const std::string& current, const std::function<void(const std::string&)>& choose) {
    std::vector<MenuItem> items;
    items.reserve(options.size());
    for (const auto& [value, title] : options) {
        items.push_back(MenuItem::item(title, [choose, value] { choose(value); }, value == current));
    }
    return items;
}

} // namespace

Menu::Menu(Controller& controller) : c_(controller) {
    tray_ = platform::Tray::create([this] { return build(); });
    tick();
}

void Menu::tick() {
    bool permissions = platform::screenRecordingAllowed() && platform::accessibilityAllowed();
    tray_->setState(!permissions                ? platform::TrayState::Warning
                    : c_.streamStatus().empty() ? platform::TrayState::Idle
                                                : platform::TrayState::Streaming);
    // With no tablet chosen, offer the first Android device that shows up.
    if (c_.chosenSerial().empty()) {
        for (const auto& t : c_.tablets()) {
            if (!offered_.insert(t.serial).second) continue;
            if (platform::ask(std::format("Use “{}” as a second screen?", t.name),
                              "Spanly will switch it into USB accessory mode and show a second screen on it. Install "
                              "the Spanly app on the device first. You can change the device later from the menu.",
                              "Use as Second Screen", "Not Now"))
                c_.chooseTablet(t);
            break;
        }
    }
    platform::runAfter(2, [this] { tick(); });
}

void Menu::onboardIfNeeded() {
    auto& store = platform::Store::shared();
    if (!store.boolean("onboarded").value_or(false)) {
        store.set("onboarded", true);
        platform::setLoginItem(true); // open at login by default; toggle in the menu
    }
    bool screen = platform::screenRecordingAllowed(), access = platform::accessibilityAllowed();
    if (screen && access) return;
    std::string missing;
    if (!screen) missing += "• Screen Recording: to show your screen on the tablet\n";
    if (!access) missing += "• Accessibility: to turn touches on the tablet into clicks and scrolls\n";
    platform::runAfter(0.5, [missing, screen, access] {
        if (!platform::ask("Spanly needs your permission",
                           missing + "\nTurn Spanly on in the System Settings pages that open next. If macOS asks "
                                     "to quit and reopen Spanly, choose Quit & Reopen.",
                           "Open System Settings", "Later"))
            return;
        if (!screen) platform::requestScreenRecording();
        if (!access) platform::requestAccessibility();
    });
}

std::vector<MenuItem> Menu::build() {
    auto& s = c_.settings();
    std::vector<MenuItem> m;

    // Status
    auto streams = c_.streamStatus();
    for (const auto& [tablet, detail] : streams) {
        m.push_back(MenuItem::label("Streaming to " + tablet, true));
        m.push_back(MenuItem::label(detail));
    }
    if (streams.empty() && !c_.chosenSerial().empty()) {
        m.push_back(MenuItem::label("Waiting for " + s.deviceName().value_or("the tablet"), true));
        m.push_back(MenuItem::label("Connect it with USB; Spanly opens on it by itself"));
    } else if (streams.empty()) {
        m.push_back(MenuItem::label("No tablet chosen", true));
        m.push_back(MenuItem::label("Connect an Android tablet with USB"));
    }

    // Permissions
    if (!platform::screenRecordingAllowed() || !platform::accessibilityAllowed()) m.push_back(MenuItem::separator());
    if (!platform::screenRecordingAllowed())
        m.push_back(MenuItem::item("⚠︎ Allow Screen Recording…", [] { platform::requestScreenRecording(); }));
    if (!platform::accessibilityAllowed())
        m.push_back(MenuItem::item("⚠︎ Allow Accessibility (for touch)…", [] { platform::requestAccessibility(); }));

    if (auto p = c_.primary(); p && p->link()->connected() && platform::supports(platform::Feature::TabletWindow)) {
        m.push_back(MenuItem::separator());
        m.push_back(c_.sharing()
                        ? MenuItem::item("Hide Tablet Screen", [this] { c_.hideTabletScreen(); })
                        : MenuItem::item("Show Tablet Screen on Computer…", [this] { c_.showTabletScreen(); }));
    }
    m.push_back(MenuItem::separator());
    m.push_back(MenuItem::submenu("Tablet", tabletItems()));
    m.push_back(MenuItem::submenu(
        "Mode", choices({{"extend", "Extend (separate second screen)"}, {"mirror", "Mirror (same as the main screen)"}},
                        s.mode() == Mode::Mirror ? "mirror" : "extend", [this](const std::string& v) {
                            Mode mode = v == "mirror" ? Mode::Mirror : Mode::Extend;
                            if (mode == c_.settings().mode()) return;
                            c_.settings().setMode(mode);
                            c_.applySettings(true);
                        })));
    std::string quality = s.quality() == Quality::Retina     ? "retina"
                          : s.quality() == Quality::Standard ? "standard"
                                                             : "auto";
    m.push_back(MenuItem::submenu(
        "Resolution",
        choices({{"auto", "Automatic"}, {"retina", "Retina (sharpest)"}, {"standard", "Standard (lightest)"}}, quality,
                [this, quality](const std::string& v) {
                    if (v == quality) return;
                    c_.settings().setQuality(v == "retina"     ? Quality::Retina
                                             : v == "standard" ? Quality::Standard
                                                               : Quality::Auto);
                    c_.applySettings(true);
                })));
    m.push_back(MenuItem::submenu("Position", choices({{"right", "Right"},
                                                       {"left", "Left"},
                                                       {"above", "Above"},
                                                       {"below", "Below"},
                                                       {"keep", "Don't Move (arrange in System Settings)"}},
                                                      s.position(), [this](const std::string& v) {
                                                          c_.settings().setPosition(v);
                                                          c_.applySettings(false);
                                                      })));
    m.push_back(MenuItem::item("Allow Wi-Fi Connection", [this] { c_.setWifi(!c_.settings().wifi()); }, s.wifi()));
    if (s.wifi()) {
        m.push_back(MenuItem::label("  Tablets plugged in once can then connect wirelessly"));
        m.push_back(MenuItem::item("  Forget Paired Tablets", [this] { c_.forgetPairedTablets(); }));
    }
    std::string sound = s.sound() == Sound::Tablet ? "tablet" : s.sound() == Sound::Both ? "both" : "mac";
    if (platform::supports(platform::Feature::Sound))
        m.push_back(MenuItem::submenu("Sound", choices({{"mac", "Computer Only"},
                                                        {"both", "Computer and Tablet"},
                                                        {"tablet", "Tablet Only (mutes the computer)"}},
                                                       sound, [this, sound](const std::string& v) {
                                                           bool capture =
                                                               (sound == "mac") != (v == "mac"); // sound capture on/off
                                                           c_.settings().setSound(v == "tablet" ? Sound::Tablet
                                                                                  : v == "both" ? Sound::Both
                                                                                                : Sound::Mac);
                                                           c_.updateSpeakers();
                                                           if (capture) c_.restartCapture();
                                                       })));
    if (!platform::supports(platform::Feature::Microphone)) {
    } else if (platform::microphoneInstalled()) {
        m.push_back(MenuItem::item(
            "Use Tablet as Microphone",
            [this] {
                c_.settings().setTabletMicrophone(!c_.settings().tabletMicrophone());
                c_.updateMicrophone();
            },
            s.tabletMicrophone()));
    } else {
        m.push_back(MenuItem::item("Install Spanly Microphone…", [] { platform::installMicrophone(); }));
    }
    m.push_back(MenuItem::item(
        "Return Pointer After Touch",
        [this] {
            c_.settings().setRestoreCursor(!c_.settings().restoreCursor());
            c_.applySettings(false);
        },
        s.restoreCursor()));

    m.push_back(MenuItem::separator());
    m.push_back(MenuItem::item(
        "Open at Login", [] { platform::setLoginItem(!platform::loginItemEnabled()); }, platform::loginItemEnabled()));
    m.push_back(MenuItem::item("Show Log", [] { platform::openFile(platform::logFilePath()); }));
    m.push_back(MenuItem::label("Version " + std::string(kVersion)));
    m.push_back(MenuItem::separator());
    MenuItem quit = MenuItem::item("Quit Spanly", [] { platform::quit(); });
    quit.key = "q";
    m.push_back(quit);
    return m;
}

std::vector<MenuItem> Menu::tabletItems() {
    std::string chosen = c_.chosenSerial();
    std::vector<MenuItem> items;
    bool found = false;
    for (const auto& t : c_.tablets()) {
        found = found || t.serial == chosen;
        items.push_back(MenuItem::item(t.name, [this, t] { c_.chooseTablet(t); }, t.serial == chosen));
    }
    if (!chosen.empty() && !found)
        items.insert(items.begin(),
                     MenuItem::item(c_.settings().deviceName().value_or(chosen) + " (not connected)", {}, true, false));
    if (items.empty()) items.push_back(MenuItem::item("No Android devices connected", {}, false, false));
    return items;
}

} // namespace spanly
