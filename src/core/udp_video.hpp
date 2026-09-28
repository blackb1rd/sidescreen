#pragma once
// Video over UDP for Wi-Fi links (PROTOCOL.md, "Video over UDP"). The encrypted TCP connection
// keeps everything else; frames go as datagrams, so a lost packet costs one frame (the tablet
// asks for a keyframe) instead of stalling everything behind it, as TCP would.
//
// Registration (tablet -> host, port 27185): "SPU1" + the 16-byte token from UDP_OFFER.
// Video (host -> tablet): seq u64 BE + AES-256-GCM(type u8, id u32, index u16, count u16, chunk),
// nonce = seq, key = HKDF(secret, nonces, "spanly udp s2c").

#include "core/crypto.hpp"
#include "core/net.hpp"

#include <atomic>
#include <functional>
#include <map>
#include <mutex>
#include <thread>

namespace spanly {

constexpr uint16_t kUdpVideoPort = 27185;
constexpr size_t kUdpChunk = 1150; // keeps datagrams under ~1200 bytes: no IP fragmentation

/// The host end: one socket for every tablet.
class UdpEndpoint {
public:
    static UdpEndpoint& shared();
    bool available() const { return enabled_ && socket_.valid(); }
    /// Off: Wi-Fi video stays on TCP (--no-udp).
    void setEnabled(bool on) { enabled_ = on; }
    /// Wait for a tablet presenting `token`; `attached` runs (on the receive thread) when it does.
    void expect(const Bytes& token, std::function<void(const net::Address&)> attached);
    void forget(const Bytes& token);
    bool send(const net::Address& to, ByteView datagram) const { return net::sendTo(socket_, to, datagram); }

private:
    UdpEndpoint();
    void receiveLoop();

    net::Socket socket_;
    std::atomic<bool> enabled_{true};
    std::mutex m_;
    std::map<Bytes, std::function<void(const net::Address&)>> expected_;
    std::thread thread_;
};

/// Splits a message into encrypted datagrams for one tablet.
class UdpPacketizer {
public:
    explicit UdpPacketizer(const crypto::Key& key) : key_(key) {}
    std::vector<Bytes> packetize(uint8_t type, uint32_t id, ByteView payload);

private:
    crypto::Key key_;
    std::atomic<uint64_t> seq_{1};
};

} // namespace spanly
