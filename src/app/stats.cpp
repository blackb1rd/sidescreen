#include "app/stats.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <format>

namespace spanly {

void Stats::frame(size_t bytes) {
    std::scoped_lock l(m_);
    ++frames_;
    bytes_ += bytes;
}

void Stats::drop() {
    std::scoped_lock l(m_);
    ++dropped_;
}

void Stats::latency(double ms) {
    std::scoped_lock l(m_);
    ++acks_;
    latencyMs_ += ms;
    maxLatencyMs_ = std::max(maxLatencyMs_, ms);
}

void Stats::audio(const Bytes& pcm) {
    int peak = 0;
    for (size_t i = 0; i + 1 < pcm.size(); i += 2) {
        int16_t s;
        std::memcpy(&s, pcm.data() + i, 2);
        peak = std::max(peak, std::abs(int(s)));
    }
    std::scoped_lock l(m_);
    audioPeak_ = std::max(audioPeak_, peak);
}

std::optional<std::string> Stats::report(double seconds) {
    std::scoped_lock l(m_);
    std::optional<std::string> line;
    if (frames_ > 0) {
        line = std::format("{:.0f} fps, glass-to-decoded avg {:.1f} ms / max {:.1f} ms, {:.1f} Mbit/s, {} skipped",
                           double(frames_) / seconds, acks_ ? latencyMs_ / double(acks_) : 0.0, maxLatencyMs_,
                           double(bytes_) * 8 / seconds / 1e6, dropped_);
        if (audioPeak_ > 0) *line += std::format(", audio peak {}", audioPeak_);
    } else if (audioPeak_ > 0) {
        line = std::format("audio peak {}", audioPeak_);
    }
    frames_ = bytes_ = dropped_ = acks_ = 0;
    latencyMs_ = maxLatencyMs_ = 0;
    audioPeak_ = 0;
    return line;
}

} // namespace spanly
