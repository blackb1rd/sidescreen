#include "core/tcp_link.hpp"

#include "core/log.hpp"

#include <cstring>
#include <map>
#include <thread>

namespace spanly {

using namespace std::chrono_literals;

namespace {

/// A device with an old or wrong pairing retries every half second: say so once a minute.
void logUnpaired(const std::string& peer) {
    static std::mutex m;
    static std::map<std::string, Clock::time_point> last;
    std::scoped_lock l(m);
    auto& t = last[peer];
    if (t != Clock::time_point{} && Clock::now() - t < 60s) return;
    t = Clock::now();
    log("Wi-Fi: a device that isn't paired tried to connect ({}); plug it in with USB once to pair it", peer);
}

} // namespace

TcpLink::TcpLink(net::Socket socket, Kind kind, std::optional<Bytes> secret, std::string peer)
    : socket_(std::move(socket)), kind_(kind), secret_(std::move(secret)), peer_(std::move(peer)) {
    net::setNoDelay(socket_);
}

TcpLink::~TcpLink() = default;

void TcpLink::start() {
    auto self = std::static_pointer_cast<TcpLink>(shared_from_this());
    std::thread([self] { self->readLoop(); }).detach();
    std::thread([self] { self->writeLoop(); }).detach();
}

bool TcpLink::connected() const {
    std::scoped_lock l(m_);
    return helloSeen_ && !closed_;
}

size_t TcpLink::backlog() const {
    std::scoped_lock l(m_);
    return pending_;
}

void TcpLink::send(Msg type, ByteView payload) {
    {
        std::scoped_lock l(m_);
        if (!established_ || closed_) return;
    }
    queue(encode(type, payload));
}

void TcpLink::queue(Bytes message) {
    std::scoped_lock l(m_);
    queue_.push_back(std::move(message));
    ++pending_;
    wake_.notify_one();
}

void TcpLink::close(const std::string& reason) {
    bool wasUp;
    {
        std::scoped_lock l(m_);
        if (closed_) return;
        closed_ = true;
        wasUp = helloSeen_;
        queue_.clear();
        pending_ = 0;
        wake_.notify_all();
    }
    socket_.shutdown(); // wakes the reader
    if (onClosed) onClosed();
    if (!wasUp) return;
    log("{} link closed ({})", kindName(kind_), reason);
    if (onDisconnect) onDisconnect();
}

void TcpLink::handle(const Message& m) {
    if (m.type == uint8_t(Msg::Hello)) {
        bool first;
        {
            std::scoped_lock l(m_);
            first = !helloSeen_;
            helloSeen_ = established_ = true;
        }
        if (first) log("tablet connected over {}", kindName(kind_));
    } else if (m.type == uint8_t(Msg::Standby)) {
        bool first;
        {
            std::scoped_lock l(m_);
            first = !established_;
            established_ = true;
        }
        if (first) log("tablet keeps Wi-Fi ready while on USB");
    }
    Link::handle(m);
}

void TcpLink::readLoop() {
    net::setReceiveTimeout(socket_, 1000);
    Bytes raw;
    Reader reader;
    uint8_t chunk[64 * 1024];
    bool keysReady = !secret_;
    while (true) {
        long n = net::receive(socket_, chunk, sizeof chunk);
        {
            std::scoped_lock l(m_);
            if (closed_) return;
        }
        if (n == 0) return close("connection ended");
        if (n < 0) {
            auto silent = Clock::now().time_since_epoch().count() - lastReceive_.load();
            if (Clock::duration(silent) > 4s) return close("no heartbeat");
            continue; // receive timeout: check again
        }
        lastReceive_ = Clock::now().time_since_epoch().count();
        if (!secret_) {
            reader.push(ByteView(chunk, size_t(n)));
            drain(reader);
            continue;
        }
        raw.insert(raw.end(), chunk, chunk + n);
        if (!keysReady) {
            if (raw.size() < crypto::kMagic.size() + crypto::kNonceSize) continue;
            if (!handshake(raw, *secret_)) return close("not paired");
            keysReady = true;
        }
        if (!openRecords(raw, reader)) {
            logUnpaired(peer_);
            return close("failed authentication");
        }
        drain(reader);
    }
}

/// "SPW1" + the tablet's nonce; we answer with ours and derive the session keys.
bool TcpLink::handshake(Bytes& raw, const Bytes& secret) {
    if (std::memcmp(raw.data(), crypto::kMagic.data(), crypto::kMagic.size()) != 0) return false;
    ByteView clientNonce(raw.data() + crypto::kMagic.size(), crypto::kNonceSize);
    Bytes serverNonce = crypto::random(crypto::kNonceSize);
    keys_ = crypto::serverKeys(secret, clientNonce, serverNonce);
    raw.erase(raw.begin(), raw.begin() + ptrdiff_t(crypto::kMagic.size() + crypto::kNonceSize));
    return net::sendAll(socket_, serverNonce);
}

/// Records: length u32 + AES-256-GCM(ciphertext || tag).
bool TcpLink::openRecords(Bytes& raw, Reader& reader) {
    size_t off = 0;
    while (raw.size() - off >= 4) {
        size_t len = u32At(raw, off);
        if (len > kMaxRecord) return false;
        if (raw.size() - off < 4 + len) break;
        auto plain = crypto::open(ByteView(raw.data() + off + 4, len), keys_.receive, receiveCounter_++);
        if (!plain) return false;
        reader.push(*plain);
        off += 4 + len;
    }
    raw.erase(raw.begin(), raw.begin() + ptrdiff_t(off));
    return true;
}

void TcpLink::writeLoop() {
    while (true) {
        Bytes next;
        {
            std::unique_lock l(m_);
            // An idle screen sends nothing: heartbeat every 0.5 s so the tablet knows we're here.
            if (!wake_.wait_for(l, 500ms, [&] { return closed_ || !queue_.empty(); })) {
                if (!established_) continue;
                queue_.push_back(encode(Msg::Nop));
                ++pending_;
            }
            if (closed_) return;
            next = std::move(queue_.front());
            queue_.pop_front();
        }
        Bytes wire;
        if (secret_) {
            Bytes sealed = crypto::seal(next, keys_.send, sendCounter_++);
            putU32(wire, uint32_t(sealed.size()));
            wire.insert(wire.end(), sealed.begin(), sealed.end());
        } else {
            wire = std::move(next);
        }
        bool ok = net::sendAll(socket_, wire);
        {
            std::scoped_lock l(m_);
            if (pending_ > 0) --pending_;
        }
        if (!ok) return close("write failed");
    }
}

} // namespace spanly
