#pragma once
// One tablet connection over TCP: through `adb reverse` (plain) or Wi-Fi (encrypted with keys
// derived from the pairing secret, see crypto.hpp). A reader thread turns bytes into messages;
// a writer thread sends queued ones, so callers never block on the network. Heartbeats both
// ways every 0.5 s: 4 s of silence from the tablet means it is gone.

#include "core/crypto.hpp"
#include "core/flow.hpp"
#include "core/link.hpp"
#include "core/net.hpp"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <optional>

namespace spanly {

class TcpLink : public Link {
public:
    /// `secret`: the Wi-Fi pairing secret (encrypted link); none for adb.
    TcpLink(net::Socket socket, Kind kind, std::optional<Bytes> secret, std::string peer);
    ~TcpLink() override;

    /// Start the reader and writer threads (after the callbacks are set).
    void start();
    void close(const std::string& reason);

    Kind kind() const override { return kind_; }
    bool connected() const override;
    void send(Msg type, ByteView payload = {}) override;
    size_t backlog() const override;

    /// Called once when the connection ends, whether or not the tablet said HELLO.
    std::function<void()> onClosed;

protected:
    void handle(const Message& m) override;

private:
    void readLoop();
    void writeLoop();
    bool handshake(Bytes& raw, const Bytes& secret);
    bool openRecords(Bytes& raw, Reader& reader);
    void queue(Bytes message);

    net::Socket socket_;
    const Kind kind_;
    const std::optional<Bytes> secret_;
    const std::string peer_;
    crypto::Keys keys_{};
    uint64_t sendCounter_ = 0;    // writer thread only
    uint64_t receiveCounter_ = 0; // reader thread only

    mutable std::mutex m_;
    std::condition_variable wake_;
    std::deque<Bytes> queue_;
    size_t pending_ = 0;
    bool helloSeen_ = false;
    bool established_ = false; // HELLO or STANDBY seen: heartbeats and sends may flow
    bool closed_ = false;
    std::atomic<Clock::rep> lastReceive_{Clock::now().time_since_epoch().count()};
};

} // namespace spanly
