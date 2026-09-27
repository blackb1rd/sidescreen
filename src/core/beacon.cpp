#include "core/beacon.hpp"

#include "core/net.hpp"

#include <algorithm>
#include <chrono>

namespace spanly {

void Beacon::start() {
    if (running_.exchange(true)) return;
    thread_ = std::thread([this] {
        Bytes payload{'S', 'P', 'B', '1'};
        payload.insert(payload.end(), name_.begin(),
                       name_.begin() + std::ptrdiff_t(std::min<size_t>(name_.size(), 200)));
        net::Socket v4 = net::udp(false), v6 = net::udp(true);
        while (running_) {
            for (const auto& i : net::interfaces()) {
                if (!i.ipv6 && i.hasBroadcast && v4.valid()) net::sendBroadcast(v4, i.broadcast, port_, payload);
                if (i.ipv6 && v6.valid()) net::sendAllNodes(v6, i.index, port_, payload);
            }
            for (int i = 0; i < 10 && running_; ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    });
}

void Beacon::stop() {
    if (!running_.exchange(false)) return;
    if (thread_.joinable()) thread_.join();
}

} // namespace spanly
