#include "core/net.hpp"

#include <cstring>
#include <set>

#ifdef _WIN32
#include <winsock2.h>
// (in this order: each needs the one before)
#include <ws2tcpip.h>

#include <iphlpapi.h>
#else
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace spanly::net {

#ifdef _WIN32
using socklen = int;
static int closeHandle(Handle h) {
    return closesocket(SOCKET(h));
}
static int pollOne(Handle h, int timeoutMs) {
    WSAPOLLFD p{SOCKET(h), POLLRDNORM, 0};
    return WSAPoll(&p, 1, timeoutMs);
}
#else
using socklen = socklen_t;
static int closeHandle(Handle h) {
    return ::close(h);
}
static int pollOne(Handle h, int timeoutMs) {
    pollfd p{h, POLLIN, 0};
    return ::poll(&p, 1, timeoutMs);
}
#endif

Socket& Socket::operator=(Socket&& o) noexcept {
    if (this != &o) {
        close();
        h_ = o.h_;
        o.h_ = kInvalid;
    }
    return *this;
}

void Socket::close() {
    if (h_ != kInvalid) closeHandle(h_);
    h_ = kInvalid;
}

void Socket::shutdown() const {
#ifdef _WIN32
    if (h_ != kInvalid) ::shutdown(SOCKET(h_), SD_BOTH);
#else
    if (h_ != kInvalid) ::shutdown(h_, SHUT_RDWR);
#endif
}

void init() {
#ifdef _WIN32
    static bool done = [] {
        WSADATA d;
        return WSAStartup(MAKEWORD(2, 2), &d) == 0;
    }();
    (void)done;
#endif
}

Socket listenTcp(uint16_t port, bool loopbackOnly) {
    init();
    int one = 1, zero = 0;
    if (loopbackOnly) {
        Socket s(Handle(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)));
        if (!s.valid()) return {};
        setsockopt(s.handle(), SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), sizeof one);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(port);
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::bind(s.handle(), reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 || ::listen(s.handle(), 4) != 0)
            return {};
        return s;
    }
    Socket s(Handle(::socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP)));
    if (!s.valid()) return {};
    setsockopt(s.handle(), SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), sizeof one);
    setsockopt(s.handle(), IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&zero), sizeof zero);
    sockaddr_in6 a{};
    a.sin6_family = AF_INET6;
    a.sin6_port = htons(port);
    a.sin6_addr = in6addr_any;
    if (::bind(s.handle(), reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 || ::listen(s.handle(), 8) != 0) return {};
    return s;
}

Socket accept(const Socket& listener, int timeoutMs, std::string* peer) {
    if (pollOne(listener.handle(), timeoutMs) <= 0) return {};
    sockaddr_storage a{};
    socklen len = sizeof a;
    Socket s(Handle(::accept(listener.handle(), reinterpret_cast<sockaddr*>(&a), &len)));
    if (s.valid() && peer) {
        char text[INET6_ADDRSTRLEN] = {};
        const void* ip = a.ss_family == AF_INET6
                             ? static_cast<const void*>(&reinterpret_cast<sockaddr_in6*>(&a)->sin6_addr)
                             : static_cast<const void*>(&reinterpret_cast<sockaddr_in*>(&a)->sin_addr);
        inet_ntop(a.ss_family, ip, text, sizeof text);
        *peer = text;
    }
    return s;
}

void setNoDelay(const Socket& s) {
    int one = 1;
    setsockopt(s.handle(), IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof one);
#ifdef SO_NOSIGPIPE
    setsockopt(s.handle(), SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
}

void setReceiveTimeout(const Socket& s, int ms) {
#ifdef _WIN32
    DWORD t = DWORD(ms);
    setsockopt(s.handle(), SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&t), sizeof t);
#else
    timeval t{ms / 1000, (ms % 1000) * 1000};
    setsockopt(s.handle(), SOL_SOCKET, SO_RCVTIMEO, &t, sizeof t);
#endif
}

bool sendAll(const Socket& s, ByteView data) {
#ifdef MSG_NOSIGNAL
    constexpr int flags = MSG_NOSIGNAL;
#else
    constexpr int flags = 0;
#endif
    size_t off = 0;
    while (off < data.size()) {
        auto n = ::send(s.handle(), reinterpret_cast<const char*>(data.data() + off), int(data.size() - off), flags);
        if (n <= 0) return false;
        off += size_t(n);
    }
    return true;
}

long receive(const Socket& s, uint8_t* buf, size_t size) {
    return long(::recv(s.handle(), reinterpret_cast<char*>(buf), int(size), 0));
}

std::vector<Interface> interfaces() {
    std::vector<Interface> out;
#ifdef _WIN32
    ULONG size = 16 * 1024;
    std::vector<uint8_t> buf(size);
    auto* list = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
    if (GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_SKIP_MULTICAST, nullptr, list, &size) ==
        ERROR_BUFFER_OVERFLOW) {
        buf.resize(size);
        list = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
        if (GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_SKIP_MULTICAST, nullptr, list, &size) !=
            NO_ERROR)
            return out;
    }
    for (auto* a = list; a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        std::set<int> families;
        for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
            int family = u->Address.lpSockaddr->sa_family;
            if (!families.insert(family).second) continue;
            Interface i{a->AdapterName, family == AF_INET6 ? unsigned(a->Ipv6IfIndex) : unsigned(a->IfIndex),
                        family == AF_INET6};
            if (family == AF_INET) {
                auto* sin = reinterpret_cast<sockaddr_in*>(u->Address.lpSockaddr);
                uint32_t ip = ntohl(sin->sin_addr.s_addr);
                uint32_t mask = u->OnLinkPrefixLength ? ~0u << (32 - u->OnLinkPrefixLength) : 0;
                uint32_t b = ip | ~mask;
                i.hasBroadcast = true;
                i.broadcast = {uint8_t(b >> 24), uint8_t(b >> 16), uint8_t(b >> 8), uint8_t(b)};
            }
            out.push_back(i);
        }
    }
#else
    ifaddrs* list = nullptr;
    if (getifaddrs(&list) != 0) return out;
    std::set<std::pair<std::string, bool>> seen;
    for (ifaddrs* a = list; a; a = a->ifa_next) {
        if (!a->ifa_addr || !(a->ifa_flags & IFF_UP) || (a->ifa_flags & IFF_LOOPBACK)) continue;
        int family = a->ifa_addr->sa_family;
        if (family != AF_INET && family != AF_INET6) continue;
        bool v6 = family == AF_INET6;
        if (v6 && !(a->ifa_flags & IFF_MULTICAST)) continue;
        if (!seen.insert({a->ifa_name, v6}).second) continue;
        Interface i{a->ifa_name, if_nametoindex(a->ifa_name), v6};
        if (!v6 && (a->ifa_flags & IFF_BROADCAST) && a->ifa_broadaddr) {
            auto* b = reinterpret_cast<sockaddr_in*>(a->ifa_broadaddr);
            i.hasBroadcast = true;
            std::memcpy(i.broadcast.data(), &b->sin_addr, 4);
        }
        out.push_back(i);
    }
    freeifaddrs(list);
#endif
    return out;
}

Socket udp(bool ipv6) {
    init();
    Socket s(Handle(::socket(ipv6 ? AF_INET6 : AF_INET, SOCK_DGRAM, IPPROTO_UDP)));
    if (s.valid() && !ipv6) {
        int one = 1;
        setsockopt(s.handle(), SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&one), sizeof one);
    }
    return s;
}

Socket bindUdp(uint16_t port) {
    init();
    Socket s(Handle(::socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP)));
    if (!s.valid()) return {};
    int zero = 0, size = 1 << 20;
    setsockopt(s.handle(), IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&zero), sizeof zero);
    setsockopt(s.handle(), SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&size), sizeof size);
    sockaddr_in6 a{};
    a.sin6_family = AF_INET6;
    a.sin6_port = htons(port);
    a.sin6_addr = in6addr_any;
    if (::bind(s.handle(), reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) return {};
    return s;
}

long receiveFrom(const Socket& s, uint8_t* buf, size_t size, Address& from, int timeoutMs) {
    if (pollOne(s.handle(), timeoutMs) <= 0) return 0;
    socklen len = sizeof from.raw;
    auto n = ::recvfrom(s.handle(), reinterpret_cast<char*>(buf), int(size), 0,
                        reinterpret_cast<sockaddr*>(from.raw.data()), &len);
    from.length = uint32_t(len);
    return long(n);
}

bool sendTo(const Socket& s, const Address& to, ByteView data) {
    return ::sendto(s.handle(), reinterpret_cast<const char*>(data.data()), int(data.size()), 0,
                    reinterpret_cast<const sockaddr*>(to.raw.data()), socklen(to.length)) == long(data.size());
}

void sendBroadcast(const Socket& s, const std::array<uint8_t, 4>& address, uint16_t port, ByteView data) {
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    std::memcpy(&a.sin_addr, address.data(), 4);
    ::sendto(s.handle(), reinterpret_cast<const char*>(data.data()), int(data.size()), 0,
             reinterpret_cast<sockaddr*>(&a), sizeof a);
}

void sendAllNodes(const Socket& s, unsigned index, uint16_t port, ByteView data) {
    sockaddr_in6 a{};
    a.sin6_family = AF_INET6;
    a.sin6_port = htons(port);
    inet_pton(AF_INET6, "ff02::1", &a.sin6_addr);
    a.sin6_scope_id = index;
    ::sendto(s.handle(), reinterpret_cast<const char*>(data.data()), int(data.size()), 0,
             reinterpret_cast<sockaddr*>(&a), sizeof a);
}

} // namespace spanly::net
