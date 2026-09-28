// TcpLink end to end over loopback, with the test playing the tablet: the reader and writer
// threads, the Wi-Fi handshake and records, and closing. Run under ThreadSanitizer in CI.
#include "core/records.hpp"
#include "core/tcp_link.hpp"
#include "test.hpp"

#include <chrono>
#include <condition_variable>
#include <mutex>

using namespace spanly;
using namespace std::chrono_literals;

namespace {

/// What the link reported, from its own threads.
struct Events {
    std::mutex m;
    std::condition_variable changed;
    std::optional<Hello> hello;
    std::vector<uint32_t> acks;
    bool closed = false;

    template <class Done>
    bool wait(Done done) {
        std::unique_lock l(m);
        return changed.wait_for(l, 5s, done);
    }
    template <class Update>
    void update(Update u) {
        std::scoped_lock l(m);
        u();
        changed.notify_all();
    }
};

struct Connection {
    std::shared_ptr<TcpLink> link;
    net::Socket tablet;
};

Connection connect(const std::optional<Bytes>& secret, Events& ev) {
    net::Socket listener = net::listenTcp(0, true);
    net::Socket tablet = net::connectLoopback(net::localPort(listener));
    net::Socket host = net::accept(listener, 2000);
    auto kind = secret ? Link::Kind::Wifi : Link::Kind::Adb;
    auto link = std::make_shared<TcpLink>(std::move(host), kind, secret, "test");
    link->onHello = [&ev](const Hello& h) { ev.update([&] { ev.hello = h; }); };
    link->onAck = [&ev](uint32_t id) { ev.update([&] { ev.acks.push_back(id); }); };
    link->onClosed = [&ev] { ev.update([&] { ev.closed = true; }); };
    link->start();
    net::setReceiveTimeout(tablet, 5000);
    return {link, std::move(tablet)};
}

Bytes hello() {
    Bytes p;
    for (uint32_t v : {2560U, 1600U, 320U, 0U})
        putU32(p, v);
    return encode(Msg::Hello, p);
}

Bytes ack(uint32_t id) {
    Bytes p;
    putU32(p, id);
    return encode(Msg::Ack, p);
}

bool receiveExactly(const net::Socket& s, uint8_t* out, size_t n) {
    for (size_t got = 0; got < n;) {
        long r = net::receive(s, out + got, n - got);
        if (r <= 0) return false;
        got += size_t(r);
    }
    return true;
}

/// The next ACK the host sends (skipping heartbeats). ACK is a type the Reader accepts, so the
/// test uses it for host -> tablet messages.
std::optional<uint32_t> nextAck(const net::Socket& tablet, RecordOpener* opener) {
    Reader reader;
    Bytes raw;
    uint8_t buf[4096];
    for (auto deadline = Clock::now() + 5s; Clock::now() < deadline;) {
        while (auto m = reader.next())
            if (m->type == uint8_t(Msg::Ack)) return u32At(m->payload, 0);
        long n = net::receive(tablet, buf, sizeof buf);
        if (n <= 0) return std::nullopt;
        if (!opener) {
            reader.push(ByteView(buf, size_t(n)));
            continue;
        }
        raw.insert(raw.end(), buf, buf + n);
        if (!opener->open(raw, reader)) return std::nullopt;
    }
    return std::nullopt;
}

Bytes secret(uint8_t fill) {
    return Bytes(32, fill);
}

/// The tablet's side of the Wi-Fi handshake: its keys, or none if the host hung up.
std::optional<crypto::Keys> handshake(const net::Socket& tablet, const Bytes& key) {
    Bytes first(crypto::kMagic.begin(), crypto::kMagic.end());
    Bytes clientNonce = crypto::random(crypto::kNonceSize);
    first.insert(first.end(), clientNonce.begin(), clientNonce.end());
    if (!net::sendAll(tablet, first)) return std::nullopt;
    Bytes serverNonce(crypto::kNonceSize);
    if (!receiveExactly(tablet, serverNonce.data(), serverNonce.size())) return std::nullopt;
    crypto::Keys host = crypto::serverKeys(key, clientNonce, serverNonce);
    return crypto::Keys{host.receive, host.send}; // the tablet's send key is the host's receive key
}

} // namespace

TEST(adbLinkCarriesMessagesBothWays) {
    Events ev;
    auto [link, tablet] = connect(std::nullopt, ev);
    Bytes both = hello();
    Bytes second = ack(7);
    both.insert(both.end(), second.begin(), second.end());
    // Split mid-message: the reader must put it back together.
    CHECK(net::sendAll(tablet, ByteView(both).first(10)));
    CHECK(net::sendAll(tablet, ByteView(both).subspan(10)));
    CHECK(ev.wait([&] { return ev.hello && !ev.acks.empty(); }));
    CHECK(ev.hello && ev.hello->w == 2560 && ev.hello->dpi == 320);
    CHECK(link->connected());

    link->send(Msg::Ack, ByteView(ack(42)).subspan(kHeader));
    CHECK(nextAck(tablet, nullptr) == 42U);

    tablet.close();
    CHECK(ev.wait([&] { return ev.closed; }));
    CHECK(!link->connected());
}

TEST(wifiLinkEncryptsBothWays) {
    Events ev;
    Bytes key = secret(7);
    auto [link, tablet] = connect(key, ev);
    auto keys = handshake(tablet, key);
    CHECK(keys.has_value());
    if (!keys) return;
    RecordSealer sealer(keys->send);
    RecordOpener opener(keys->receive);
    CHECK(net::sendAll(tablet, sealer.seal(hello())));
    CHECK(net::sendAll(tablet, sealer.seal(ack(9))));
    CHECK(ev.wait([&] { return ev.hello && !ev.acks.empty(); }));
    CHECK(ev.acks.front() == 9U);

    link->send(Msg::Ack, ByteView(ack(43)).subspan(kHeader));
    CHECK(nextAck(tablet, &opener) == 43U);

    Bytes tampered = sealer.seal(ack(10));
    tampered.back() ^= 1U;
    CHECK(net::sendAll(tablet, tampered));
    CHECK(ev.wait([&] { return ev.closed; }));
    CHECK(ev.acks.size() == 1);
}

TEST(wifiLinkRejectsAnotherPairing) {
    Events ev;
    auto [link, tablet] = connect(secret(7), ev);
    auto keys = handshake(tablet, secret(8)); // the host still answers with its nonce
    CHECK(keys.has_value());
    if (!keys) return;
    RecordSealer sealer(keys->send);
    CHECK(net::sendAll(tablet, sealer.seal(hello())));
    CHECK(ev.wait([&] { return ev.closed; }));
    CHECK(!ev.hello);
}

TEST(wifiLinkRejectsAPlainConnection) {
    Events ev;
    auto [link, tablet] = connect(secret(7), ev);
    Bytes plain = hello();
    plain.resize(crypto::kMagic.size() + crypto::kNonceSize);
    CHECK(net::sendAll(tablet, plain));
    CHECK(ev.wait([&] { return ev.closed; }));
}

TEST(sealedRecordsSurviveAnySplit) {
    crypto::Key key{};
    key[0] = 1;
    RecordSealer sealer(key);
    Bytes stream;
    for (uint32_t id = 0; id < 3; ++id) {
        Bytes r = sealer.seal(ack(id));
        stream.insert(stream.end(), r.begin(), r.end());
    }
    for (size_t step = 1; step <= stream.size(); step += 7) {
        RecordOpener opener(key);
        Reader reader;
        Bytes raw;
        std::vector<uint32_t> ids;
        for (size_t off = 0; off < stream.size(); off += step) {
            size_t n = std::min(step, stream.size() - off);
            raw.insert(raw.end(), stream.begin() + ptrdiff_t(off), stream.begin() + ptrdiff_t(off + n));
            CHECK(opener.open(raw, reader));
            while (auto m = reader.next())
                ids.push_back(u32At(m->payload, 0));
        }
        CHECK((ids == std::vector<uint32_t>{0, 1, 2}));
    }
}
