#pragma once
// Keeps `adb reverse` alive across replugs (the slow fallback link), launches the tablet's app,
// and mirrors the computer's sleep/wake on the tablet. Optional: without adb, the USB accessory
// and Wi-Fi links work on their own.

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace spanly {

class Adb {
public:
    static constexpr const char* kAppComponent = "com.caigenix.spanly/.MainActivity";

    /// Null if adb isn't installed (ADB, the Android SDK, Homebrew or /usr/local).
    static std::unique_ptr<Adb> find(uint16_t port);
    ~Adb();

    void startWatching();
    /// USB serial number of the tablet adb sees (the last one known).
    std::optional<std::string> serial() const;
    void sleepTablet();
    void wakeTablet();

private:
    Adb(std::string path, uint16_t port) : path_(std::move(path)), port_(port) {}
    std::string run(const std::vector<std::string>& args) const;
    void tick();
    void launchApp() const;
    void async(std::function<void()> fn);

    const std::string path_;
    const uint16_t port_;
    std::atomic<bool> running_{false};
    std::thread thread_;
    bool ready_ = false; // watcher thread only
    mutable std::mutex m_;
    std::optional<std::string> serial_;
};

} // namespace spanly
