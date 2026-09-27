#include "app/adb.hpp"

#include "core/log.hpp"
#include "platform/platform.hpp"

#include <cctype>
#include <chrono>
#include <cstdlib>
#include <vector>

namespace spanly {

namespace {
std::string trim(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
        s.pop_back();
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
        s.erase(s.begin());
    return s;
}
} // namespace

std::unique_ptr<Adb> Adb::find(uint16_t port) {
    std::vector<std::string> candidates;
    if (const char* env = std::getenv("ADB")) candidates.emplace_back(env);
    std::string home = platform::homeDirectory();
#ifdef _WIN32
    candidates.push_back(home + "\\AppData\\Local\\Android\\Sdk\\platform-tools\\adb.exe");
#else
    candidates.push_back(home + "/Library/Android/sdk/platform-tools/adb");
    candidates.push_back(home + "/Android/Sdk/platform-tools/adb");
    candidates.emplace_back("/opt/homebrew/bin/adb");
    candidates.emplace_back("/usr/local/bin/adb");
    candidates.emplace_back("/usr/bin/adb");
#endif
    for (const auto& c : candidates) {
        if (platform::isExecutable(c)) return std::unique_ptr<Adb>(new Adb(c, port));
    }
    return nullptr;
}

Adb::~Adb() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
}

std::string Adb::run(const std::vector<std::string>& args) const {
    return platform::runProcess(path_, args);
}

void Adb::startWatching() {
    running_ = true;
    thread_ = std::thread([this] {
        while (running_) {
            tick();
            for (int i = 0; i < 20 && running_; ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    });
}

void Adb::tick() {
    if (trim(run({"get-state"})) != "device") {
        if (ready_) log("tablet unplugged");
        ready_ = false;
        return;
    }
    if (std::string sn = trim(run({"get-serialno"})); !sn.empty()) {
        std::scoped_lock l(m_);
        serial_ = sn; // keep the last known serial
    }
    std::string rule = "tcp:" + std::to_string(port_);
    if (run({"reverse", "--list"}).find(rule) == std::string::npos) {
        run({"reverse", rule, rule});
        log("adb reverse {} set up", rule);
        ready_ = false;
    }
    if (!ready_) {
        ready_ = true;
        launchApp();
    }
}

std::optional<std::string> Adb::serial() const {
    std::scoped_lock l(m_);
    return serial_;
}

void Adb::launchApp() const {
    run({"shell", "am", "start", "-n", kAppComponent});
}

void Adb::async(std::function<void()> fn) {
    std::thread(std::move(fn)).detach();
}

void Adb::sleepTablet() {
    async([this] { run({"shell", "input", "keyevent", "KEYCODE_SLEEP"}); });
}

void Adb::wakeTablet() {
    async([this] {
        run({"shell", "input", "keyevent", "KEYCODE_WAKEUP"});
        launchApp();
    });
}

} // namespace spanly
