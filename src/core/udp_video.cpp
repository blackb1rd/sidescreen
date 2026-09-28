#include "core/udp_video.hpp"

#include "core/log.hpp"

#include <algorithm>
#include <cstring>

namespace spanly {

UdpEndpoint& UdpEndpoint::shared() {
    static UdpEndpoint endpoint;
    return endpoint;
}

UdpEndpoint::UdpEndpoint() : socket_(net::bindUdp(kUdpVideoPort)) {
    if (!socket_.valid()) {
        log("could not listen on UDP port {}: Wi-Fi video stays on TCP", kUdpVideoPort);
        return;
    }
    thread_ = std::thread([this] { receiveLoop(); });
    thread_.detach(); // lives as long as the app
}

std::optional<UdpEndpoint::Token> UdpEndpoint::token(ByteView bytes) {
    if (bytes.size() != kUdpTokenSize) return std::nullopt;
    Token t{};
    std::ranges::copy(bytes, t.begin());
    return t;
}

void UdpEndpoint::expect(const Bytes& token, std::function<void(const net::Address&)> attached) {
    auto t = UdpEndpoint::token(token);
    if (!t) return;
    std::scoped_lock l(m_);
    expected_[*t] = std::move(attached);
}

void UdpEndpoint::forget(const Bytes& token) {
    auto t = UdpEndpoint::token(token);
    if (!t) return;
    std::scoped_lock l(m_);
    expected_.erase(*t);
}

/// Registrations only: "SPU1" + token. Tablets repeat them until UDP_READY, then as keepalives.
void UdpEndpoint::receiveLoop() {
    uint8_t buf[64];
    while (true) {
        net::Address from;
        long n = net::receiveFrom(socket_, buf, sizeof buf, from, 1000);
        if (n != 4 + long(kUdpTokenSize) || std::memcmp(buf, "SPU1", 4) != 0) continue;
        auto t = token(ByteView(buf + 4, kUdpTokenSize));
        if (!t) continue;
        std::function<void(const net::Address&)> attached;
        {
            std::scoped_lock l(m_);
            auto it = expected_.find(*t);
            if (it == expected_.end()) continue;
            attached = it->second;
        }
        attached(from);
    }
}

std::vector<Bytes> UdpPacketizer::packetize(uint8_t type, uint32_t id, ByteView payload) {
    size_t count = std::max<size_t>(1, (payload.size() + kUdpChunk - 1) / kUdpChunk);
    std::vector<Bytes> out;
    out.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        size_t off = i * kUdpChunk, len = std::min(kUdpChunk, payload.size() - off);
        Bytes plain;
        plain.reserve(9 + len);
        plain.push_back(type);
        putU32(plain, id);
        plain.push_back(uint8_t(i >> 8U));
        plain.push_back(uint8_t(i));
        plain.push_back(uint8_t(count >> 8U));
        plain.push_back(uint8_t(count));
        plain.insert(plain.end(), payload.begin() + ptrdiff_t(off), payload.begin() + ptrdiff_t(off + len));
        uint64_t seq = seq_++;
        Bytes datagram;
        datagram.reserve(8 + plain.size() + crypto::kTagSize);
        for (int b = 7; b >= 0; --b)
            datagram.push_back(uint8_t(seq >> (8U * unsigned(b))));
        Bytes sealed = crypto::seal(plain, key_, seq);
        datagram.insert(datagram.end(), sealed.begin(), sealed.end());
        out.push_back(std::move(datagram));
    }
    return out;
}

} // namespace spanly
