#pragma once
// Counters for --stats, shared by the encoder output thread and the logger.

#include "core/protocol.hpp"

#include <mutex>
#include <optional>
#include <string>

namespace spanly {

class Stats {
public:
    void frame(size_t bytes);
    void drop();
    void latency(double ms);
    void audio(const Bytes& pcm);
    /// One line for the last `seconds`, or nothing if idle; resets the counters.
    std::optional<std::string> report(double seconds);

private:
    std::mutex m_;
    size_t frames_ = 0, bytes_ = 0, dropped_ = 0, acks_ = 0;
    double latencyMs_ = 0, maxLatencyMs_ = 0;
    int audioPeak_ = 0;
};

} // namespace spanly
