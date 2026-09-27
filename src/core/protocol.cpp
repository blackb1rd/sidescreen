#include "core/protocol.hpp"

#include <cstring>

namespace spanly {

void putU32(Bytes& out, uint32_t v) {
    out.push_back(uint8_t(v >> 24));
    out.push_back(uint8_t(v >> 16));
    out.push_back(uint8_t(v >> 8));
    out.push_back(uint8_t(v));
}

uint32_t u32At(ByteView p, size_t i) {
    return uint32_t(p[i]) << 24 | uint32_t(p[i + 1]) << 16 | uint32_t(p[i + 2]) << 8 | uint32_t(p[i + 3]);
}

float f32At(ByteView p, size_t i) {
    uint32_t bits = u32At(p, i);
    float f;
    std::memcpy(&f, &bits, sizeof f);
    return f;
}

Bytes encode(Msg type, ByteView payload) {
    Bytes d;
    d.reserve(kHeader + payload.size());
    d.push_back(kMarker);
    d.push_back(uint8_t(type));
    putU32(d, uint32_t(payload.size()));
    d.insert(d.end(), payload.begin(), payload.end());
    return d;
}

bool plausible(uint8_t type, size_t len) {
    switch (Msg(type)) {
    case Msg::Nop: return len == 0;
    case Msg::Touch:
    case Msg::Zoom: return len == 9;
    case Msg::Hello: return len >= 12 && len <= 64;
    case Msg::Ack: return len == 4;
    case Msg::Scroll: return len == 19;
    case Msg::Pen: return len == 14;
    case Msg::Viewing: return len == 1;
    case Msg::ShareSize: return len == 12;
    case Msg::ShareConfig: return len >= 1 && len <= 4096;
    case Msg::ShareFrame: return len >= 2 && len <= (16u << 20);
    case Msg::ShareStatus: return len == 2;
    case Msg::ShareAudio: return len >= 4 && len <= 65536 && len % 4 == 0;
    case Msg::MicAudio: return len >= 2 && len <= 65536 && len % 2 == 0;
    case Msg::Standby: return len == 16;
    default: return false;
    }
}

void Reader::push(ByteView data) {
    if (start_ > 0 && start_ * 2 > buf_.size()) {
        buf_.erase(buf_.begin(), buf_.begin() + ptrdiff_t(start_));
        start_ = 0;
    }
    buf_.insert(buf_.end(), data.begin(), data.end());
}

std::optional<Message> Reader::next() {
    while (buf_.size() - start_ >= kHeader) {
        ByteView b(buf_.data() + start_, buf_.size() - start_);
        size_t len = u32At(b, 2);
        if (b[0] != kMarker || !plausible(b[1], len)) {
            ++start_;
            ++skipped;
            continue;
        }
        if (b.size() < kHeader + len) return std::nullopt;
        Message m{b[1], Bytes(b.begin() + kHeader, b.begin() + ptrdiff_t(kHeader + len))};
        start_ += kHeader + len;
        return m;
    }
    return std::nullopt;
}

Hello Hello::parse(ByteView p) {
    Hello h;
    h.w = int(u32At(p, 0));
    h.h = int(u32At(p, 4));
    h.dpi = int(u32At(p, 8));
    if (p.size() >= 16) h.caps = u32At(p, 12);
    if (p.size() >= 24) {
        h.maxW = int(u32At(p, 16));
        h.maxH = int(u32At(p, 20));
    }
    if (p.size() >= 40) {
        static const char* hex = "0123456789abcdef";
        for (size_t i = 24; i < 40; ++i) {
            h.id += hex[unsigned(p[i]) >> 4U];
            h.id += hex[unsigned(p[i]) & 15U];
        }
    }
    if (p.size() > 40) h.name.assign(reinterpret_cast<const char*>(p.data() + 40), p.size() - 40);
    return h;
}

std::vector<ByteView> nalUnits(ByteView s) {
    std::vector<std::pair<size_t, size_t>> starts; // (start code offset, payload offset)
    for (size_t i = 0; i + 3 <= s.size();) {
        if (s[i] == 0 && s[i + 1] == 0 && s[i + 2] == 1) {
            starts.emplace_back(i > 0 && s[i - 1] == 0 ? i - 1 : i, i + 3);
            i += 3;
        } else {
            ++i;
        }
    }
    std::vector<ByteView> out;
    for (size_t n = 0; n < starts.size(); ++n) {
        size_t end = n + 1 < starts.size() ? starts[n + 1].first : s.size();
        out.push_back(s.subspan(starts[n].second, end - starts[n].second));
    }
    return out;
}

} // namespace spanly
