// Milestone-1 harness: brings up the USB accessory, adb and Wi-Fi links, pairs Wi-Fi over USB,
// and logs what each tablet says. (The full app replaces this as the platform backends land.)
//
//   spanly [--serial <usb serial>] [--seconds N]

#include "core/beacon.hpp"
#include "core/crypto.hpp"
#include "core/listeners.hpp"
#include "core/log.hpp"
#include "core/usb.hpp"

#include <chrono>
#include <cstring>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

using namespace spanly;

namespace {

std::string hostName() {
    char name[256] = {};
#ifdef _WIN32
    DWORD size = sizeof name;
    GetComputerNameA(name, &size);
#else
    gethostname(name, sizeof name - 1);
#endif
    return name;
}

void wire(const std::shared_ptr<Link>& link, const Bytes& secret, const std::string& name) {
    std::weak_ptr<Link> weak = link;
    link->onHello = [weak, secret, name](const Hello& h) { // NOLINT(bugprone-exception-escape)
        auto l = weak.lock();
        if (!l) return;
        log("HELLO over {}: {}x{} @ {} dpi, caps {}, decoder max {}x{}, id {}, name \"{}\"", Link::kindName(l->kind()),
            h.w, h.h, h.dpi, h.caps, h.maxW, h.maxH, h.id.empty() ? "-" : h.id, h.name);
        if (l->kind() == Link::Kind::Usb) { // plugged in = trusted: hand over the Wi-Fi secret
            Bytes pair = secret;
            pair.insert(pair.end(), name.begin(), name.end());
            l->send(Msg::Pair, pair);
        }
    };
    link->onDisconnect = [] { log("tablet disconnected"); };
}

} // namespace

int main(int argc, char** argv) try { // NOLINT(bugprone-exception-escape)
    std::string serial;
    int seconds = 60;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--serial") && i + 1 < argc) serial = argv[++i];
        if (!std::strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = int(std::strtol(argv[++i], nullptr, 10));
    }
    const std::string name = hostName();
    Bytes secret = crypto::random(32);

    auto usb = UsbLink::create([&serial] { return serial; });
    if (usb && serial.empty()) {
        for (const auto& t : usb->tablets()) {
            log("found {} ({}{})", t.name, t.serial, t.accessoryMode ? ", accessory mode" : "");
            if (serial.empty()) serial = t.serial;
        }
    }
    if (usb) wire(usb, secret, name);

    TcpListener adb(TcpListener::kAdbPort, Link::Kind::Adb);
    adb.onConnection = [&](const std::shared_ptr<TcpLink>& l) { wire(l, secret, name); };
    adb.setEnabled(true);
    TcpListener wifi(TcpListener::kWifiPort, Link::Kind::Wifi, [&] { return secret; });
    wifi.onConnection = [&](const std::shared_ptr<TcpLink>& l) { wire(l, secret, name); };
    wifi.setEnabled(true);
    Beacon beacon(name, TcpListener::kWifiPort);
    beacon.start();
    log("listening (adb {}, Wi-Fi {}) as \"{}\" for {} s", TcpListener::kAdbPort, TcpListener::kWifiPort, name,
        seconds);

    std::this_thread::sleep_for(std::chrono::seconds(seconds));
    beacon.stop();
    wifi.setEnabled(false);
    adb.setEnabled(false);
    return 0;
} catch (const std::exception& e) {
    log("fatal: {}", e.what());
    return 1;
}
