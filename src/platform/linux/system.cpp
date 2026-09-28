// Linux: the GLib main loop, paths, autostart, power, and no tray yet (settings live in
// ~/.config/spanly/settings and the command line).
#include "core/env.hpp"
#include "core/log.hpp"
#include "platform/platform.hpp"

#include <glib.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <unistd.h>

namespace spanly::platform {

namespace fs = std::filesystem;

bool supports(Feature) {
    return false;
}

namespace {

GMainLoop* gLoop = nullptr;

gboolean runFunction(gpointer p) {
    std::unique_ptr<std::function<void()>> fn(static_cast<std::function<void()>*>(p));
    (*fn)();
    return G_SOURCE_REMOVE;
}

std::string xdg(const char* var, const char* fallback) {
    return env(var).value_or(homeDirectory() + fallback);
}

} // namespace

int runApp(const std::function<void()>& start) {
    gLoop = g_main_loop_new(nullptr, FALSE);
    runOnMain(start);
    g_main_loop_run(gLoop);
    return 0;
}

void quit() {
    runOnMain([] { g_main_loop_quit(gLoop); });
}

void runOnMain(std::function<void()> fn) {
    g_idle_add(runFunction, new std::function<void()>(std::move(fn)));
}

void runAfter(double seconds, std::function<void()> fn) {
    g_timeout_add(guint(seconds * 1000), runFunction, new std::function<void()>(std::move(fn)));
}

bool screenRecordingAllowed() {
    return true; // asked by the desktop portal when the stream starts
}
void requestScreenRecording() {}
bool accessibilityAllowed() {
    return true;
}
void requestAccessibility() {}

std::string computerName() {
    char name[256] = {};
    gethostname(name, sizeof name - 1);
    return name;
}

std::string configDirectory() {
    return xdg("XDG_CONFIG_HOME", "/.config") + "/spanly";
}

std::string logFilePath() {
    std::string dir = xdg("XDG_STATE_HOME", "/.local/state") + "/spanly";
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir + "/spanly.log";
}

// MARK: Menu (no tray on Linux yet)

namespace {
class NoTray : public Tray {
public:
    void setState(TrayState) override {}
};
} // namespace

// NOLINTNEXTLINE(performance-unnecessary-value-param): the interface takes ownership
std::unique_ptr<Tray> Tray::create(std::function<std::vector<MenuItem>()>) {
    log("settings: {}/settings (or command-line options, see --help)", configDirectory());
    return std::make_unique<NoTray>();
}

bool ask(const std::string& title, const std::string&, const std::string& ok, const std::string&) {
    log("{} -> {}", title, ok); // no dialog without a tray: go ahead
    return true;
}

void openFile(const std::string& path) {
    runProcess("/usr/bin/xdg-open", {path});
}

// MARK: System

// Nothing to watch: display sleep follows the desktop's own screen blanking.
// NOLINTNEXTLINE(performance-unnecessary-value-param): the interface takes ownership
void watchSystem(SystemEvents) {}

bool onBattery() {
    std::error_code ec;
    for (const auto& e : fs::directory_iterator("/sys/class/power_supply", ec)) {
        std::ifstream type(e.path() / "type"), online(e.path() / "online");
        std::string t;
        int on = 1;
        if (type >> t && t == "Mains" && online >> on) return on == 0;
    }
    return false;
}

namespace {
std::string autostartFile() {
    return configDirectory() + "/../autostart/spanly.desktop";
}
} // namespace

bool loginItemEnabled() {
    return fs::exists(autostartFile());
}

void setLoginItem(bool enabled) {
    std::error_code ec;
    if (!enabled) {
        fs::remove(autostartFile(), ec);
        return;
    }
    fs::create_directories(fs::path(autostartFile()).parent_path(), ec);
    std::ofstream(autostartFile()) << "[Desktop Entry]\nType=Application\nName=Spanly\nExec="
                                   << fs::read_symlink("/proc/self/exe", ec).string()
                                   << "\nX-GNOME-Autostart-enabled=true\n";
}

} // namespace spanly::platform
