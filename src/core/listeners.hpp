#pragma once
// Where tablets connect over TCP: the adb listener on 127.0.0.1:27183 (through `adb reverse`,
// one tablet at a time) and the Wi-Fi listener on port 27184 (paired tablets, several at once,
// encrypted). Each connection is handed out as its own TcpLink.

#include "core/tcp_link.hpp"

#include <functional>
#include <memory>
#include <thread>
#include <vector>

namespace spanly {

class TcpListener {
public:
    static constexpr uint16_t kAdbPort = 27183;
    static constexpr uint16_t kWifiPort = 27184;

    /// `secret` gives the Wi-Fi pairing secret for each new connection; null for adb (plain).
    TcpListener(uint16_t port, Link::Kind kind, std::function<Bytes()> secret = {});
    ~TcpListener();

    /// A new connection, before anything is received on it: wire up its callbacks here.
    std::function<void(const std::shared_ptr<TcpLink>&)> onConnection;

    /// Start or stop listening; stopping closes every connection. False if the port is taken.
    bool setEnabled(bool enabled);

private:
    void acceptLoop();

    const uint16_t port_;
    const Link::Kind kind_;
    const std::function<Bytes()> secret_;
    std::mutex m_;
    net::Socket socket_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::vector<std::weak_ptr<TcpLink>> links_;
};

} // namespace spanly
