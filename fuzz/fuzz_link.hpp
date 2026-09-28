#pragma once
// A Link whose every callback is set, so fuzzed messages reach every handler.

#include "core/link.hpp"
#include "core/log.hpp"

#include <cstring>

namespace spanly::fuzz {

class FuzzLink : public Link {
public:
    FuzzLink() {
        setLogToStderr(false);
        onHello = [this](const Hello& h) {
            sink_ +=
                size_t(uint32_t(h.w)) + size_t(uint32_t(h.h)) + size_t(uint32_t(h.dpi)) + h.id.size() + h.name.size();
        };
        onAck = [this](uint32_t id) { sink_ += id; };
        onTouch = [this](uint8_t a, float x, float y) { take(a, x, y); };
        onScroll = [this](uint8_t k, uint8_t p, bool l, float x, float y, float dx, float dy) {
            take(unsigned(k) + p + l, x + dx, y + dy);
        };
        onZoom = [this](int8_t d, float x, float y) { take(uint8_t(d), x, y); };
        onPen = [this](uint8_t a, uint8_t b, float x, float y, float p) { take(a + b, x + p, y); };
        onViewing = [this](bool v) { sink_ += v; };
        onShareSize = [this](int w, int h) { sink_ += size_t(uint32_t(w)) ^ size_t(uint32_t(h)); };
        onShareConfig = [this](const Bytes& b) { sink_ += b.size(); };
        onShareFrame = [this](const Bytes& b) { sink_ += b.size(); };
        onShareStatus = [this](uint8_t s, bool c) { sink_ += s + c; };
        onShareAudio = [this](const Bytes& b) { sink_ += b.size(); };
        onMicAudio = [this](const Bytes& b) { sink_ += b.size(); };
        onKeyframeRequest = [this] { ++sink_; };
    }

    void drainAll(Reader& reader) { drain(reader); }

private:
    void take(unsigned a, float x, float y) {
        uint32_t bits = 0;
        std::memcpy(&bits, &x, sizeof bits);
        sink_ += a + bits;
        std::memcpy(&bits, &y, sizeof bits);
        sink_ += bits;
    }

    size_t sink_ = 0;
};

} // namespace spanly::fuzz
