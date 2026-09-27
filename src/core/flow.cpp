#include "core/flow.hpp"

#include <algorithm>
#include <cmath>

namespace spanly {

void FlowControl::reset(uint32_t maxInFlight) {
    std::scoped_lock l(m_);
    lastAcked_ = nextId_ - 1;
    lastAck_ = Clock::now();
    sentAt_.clear();
    maxInFlight_ = maxInFlight;
}

void FlowControl::setMaxInFlight(uint32_t n) {
    std::scoped_lock l(m_);
    maxInFlight_ = n;
}

uint32_t FlowControl::registerFrame(Clock::time_point captured) {
    std::scoped_lock l(m_);
    uint32_t id = nextId_++;
    sentAt_[id] = captured;
    return id;
}

std::optional<double> FlowControl::acked(uint32_t id) {
    std::scoped_lock l(m_);
    if (int32_t(id - lastAcked_) <= 0) return std::nullopt; // not newer than the last ACK
    lastAcked_ = id;
    lastAck_ = Clock::now();
    auto it = sentAt_.find(id);
    std::optional<double> ms;
    if (it != sentAt_.end()) ms = std::chrono::duration<double, std::milli>(Clock::now() - it->second).count();
    sentAt_.erase(sentAt_.begin(), sentAt_.upper_bound(id));
    if (ms) {
        ++window_.acks;
        latencySum_ += *ms;
    }
    return ms;
}

bool FlowControl::tooManyInFlight() {
    std::scoped_lock l(m_);
    if (nextId_ - 1 - lastAcked_ < maxInFlight_) return false;
    if (Clock::now() - lastAck_ > std::chrono::milliseconds(250)) {
        lastAcked_ = nextId_ - 1;
        lastAck_ = Clock::now();
        sentAt_.clear();
        return false;
    }
    ++window_.skipped;
    return true;
}

FlowControl::Window FlowControl::takeWindow() {
    std::scoped_lock l(m_);
    Window w = window_;
    w.avgLatencyMs = w.acks ? latencySum_ / w.acks : 0;
    window_ = {};
    latencySum_ = 0;
    return w;
}

std::optional<double> adaptBitrate(double current, double max, const FlowControl::Window& w) {
    if (w.acks + w.skipped < 10) return std::nullopt; // screen idle: nothing to learn
    double target = current;
    if (w.skipped > (w.acks + w.skipped) / 5 || w.avgLatencyMs > 90) {
        target = std::max(3.0, current * 0.75);
    } else if (w.skipped == 0 && w.avgLatencyMs < 60) {
        target = std::min(max, current * 1.1);
    }
    if (std::abs(target - current) < 0.5) return std::nullopt;
    return target;
}

} // namespace spanly
