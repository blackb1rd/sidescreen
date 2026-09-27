#pragma once
// Announces this computer with a small UDP message every second on each network it is on, so
// a tablet finds it where Bonjour doesn't reach, notably when the computer has joined the
// tablet's own hotspot. Payload: "SPB1" + the name the tablet knows from pairing; an IPv4
// broadcast per network and an IPv6 all-nodes multicast (ff02::1) per interface.

#include <atomic>
#include <string>
#include <thread>

namespace spanly {

class Beacon {
public:
    explicit Beacon(std::string name, uint16_t port) : name_(std::move(name)), port_(port) {}
    ~Beacon() { stop(); }
    void start();
    void stop();

private:
    std::string name_;
    uint16_t port_;
    std::atomic<bool> running_{false};
    std::thread thread_;
};

} // namespace spanly
