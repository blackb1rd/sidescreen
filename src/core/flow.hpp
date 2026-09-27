#pragma once
// Frames sent but not yet decoded on the tablet (it ACKs each one), the latency those ACKs
// reveal, and the adaptive bitrate built on them.

#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>

namespace spanly {

using Clock = std::chrono::steady_clock;

class FlowControl {
public:
    struct Window {
        uint32_t acks = 0;
        double avgLatencyMs = 0;
        uint32_t skipped = 0;
    };

    /// Forget what's in flight (new stream). Wi-Fi's longer round trip needs more in flight.
    void reset(uint32_t maxInFlight);
    void setMaxInFlight(uint32_t n);

    /// Id for a frame about to be sent; `captured` is when its picture was taken.
    uint32_t registerFrame(Clock::time_point captured);

    /// The tablet decoded frame `id` (and so everything before it): its latency in ms.
    std::optional<double> acked(uint32_t id);

    /// True while the tablet is behind (skip this capture). Stops waiting after 250 ms without ACKs.
    bool tooManyInFlight();

    Window takeWindow();

private:
    std::mutex m_;
    uint32_t maxInFlight_ = 3;
    uint32_t nextId_ = 1;
    uint32_t lastAcked_ = 0;
    Clock::time_point lastAck_ = Clock::now();
    std::map<uint32_t, Clock::time_point> sentAt_;
    Window window_;
    double latencySum_ = 0;
};

/// Every second: lower the bitrate when frames queue up or latency climbs, and creep back up
/// when the link has headroom. Returns the new bitrate (Mbit/s) if it changed meaningfully.
std::optional<double> adaptBitrate(double current, double max, const FlowControl::Window& w);

} // namespace spanly
