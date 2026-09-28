#pragma once
// Just enough portable sockets (BSD sockets / Winsock) for the links: blocking TCP with
// threads, and the UDP beacon.

#include "core/protocol.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace spanly::net {

#ifdef _WIN32
using Handle = uintptr_t;
constexpr Handle kInvalid = ~Handle(0);
#else
using Handle = int;
constexpr Handle kInvalid = -1;
#endif

/// An owned socket (closed when destroyed).
class Socket {
public:
    Socket() = default;
    explicit Socket(Handle h) : h_(h) {}
    Socket(Socket&& o) noexcept : h_(o.h_) { o.h_ = kInvalid; }
    Socket& operator=(Socket&& o) noexcept;
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    ~Socket() { close(); }

    bool valid() const { return h_ != kInvalid; }
    Handle handle() const { return h_; }
    void close();
    /// Wake up a thread blocked reading it (the reader then sees the connection end).
    void shutdown() const;

private:
    Handle h_ = kInvalid;
};

void init(); // Winsock start-up; harmless elsewhere

/// TCP listener: loopback only (127.0.0.1), or all interfaces, IPv4 and IPv6.
Socket listenTcp(uint16_t port, bool loopbackOnly);
/// Wait up to `timeoutMs` for a connection; invalid socket on timeout.
Socket accept(const Socket& listener, int timeoutMs, std::string* peer = nullptr);

void setNoDelay(const Socket& s);
void setReceiveTimeout(const Socket& s, int ms);
bool sendAll(const Socket& s, ByteView data);
/// > 0: bytes read; 0: connection closed; < 0: error or timeout.
long receive(const Socket& s, uint8_t* buf, size_t size);

struct Interface {
    std::string name;
    unsigned index = 0;
    bool ipv6 = false;
    bool hasBroadcast = false;
    std::array<uint8_t, 4> broadcast{}; // IPv4 only
};

/// Up, non-loopback interfaces with an IPv4 or IPv6 address (one entry per address family).
std::vector<Interface> interfaces();

Socket udp(bool ipv6);

/// A peer's address (IPv4 or IPv6), as a received datagram came from.
struct Address {
    std::array<uint8_t, 128> raw{};
    uint32_t length = 0;
    bool operator==(const Address& o) const {
        return length == o.length && std::equal(raw.begin(), raw.begin() + length, o.raw.begin());
    }
};

/// A UDP socket on all interfaces, IPv4 and IPv6.
Socket bindUdp(uint16_t port);
/// Wait up to `timeoutMs` for a datagram; its size (0 on timeout) and sender.
long receiveFrom(const Socket& s, uint8_t* buf, size_t size, Address& from, int timeoutMs);
bool sendTo(const Socket& s, const Address& to, ByteView data);
void sendBroadcast(const Socket& s, const std::array<uint8_t, 4>& address, uint16_t port, ByteView data);
/// To ff02::1 (all nodes on the link) through interface `index`.
void sendAllNodes(const Socket& s, unsigned index, uint16_t port, ByteView data);

} // namespace spanly::net
