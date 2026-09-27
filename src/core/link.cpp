#include "core/link.hpp"

#include "core/log.hpp"

namespace spanly {

std::shared_ptr<Link> Link::none() {
    static auto n = std::make_shared<Link>();
    return n;
}

const char* Link::kindName(Kind k) {
    switch (k) {
    case Kind::Usb: return "USB";
    case Kind::Wifi: return "Wi-Fi";
    case Kind::Adb: return "adb";
    default: return "none";
    }
}

std::shared_ptr<TabletSession> Link::session() const {
    std::scoped_lock l(sessionLock_);
    return session_.lock();
}

void Link::setSession(const std::shared_ptr<TabletSession>& s) {
    std::scoped_lock l(sessionLock_);
    session_ = s;
}

void Link::drain(Reader& reader) {
    while (auto m = reader.next())
        handle(*m);
    if (reader.skipped > 0) {
        log("resynchronised the stream (skipped {} bytes)", reader.skipped);
        reader.skipped = 0;
    }
}

void Link::handle(const Message& m) {
    const Bytes& p = m.payload;
    switch (Msg(m.type)) {
    case Msg::Touch:
        if (onTouch) onTouch(p[0], f32At(p, 1), f32At(p, 5));
        break;
    case Msg::Scroll:
        if (onScroll) onScroll(p[0], p[1], p[2] != 0, f32At(p, 3), f32At(p, 7), f32At(p, 11), f32At(p, 15));
        break;
    case Msg::Zoom:
        if (onZoom) onZoom(int8_t(p[0]), f32At(p, 1), f32At(p, 5));
        break;
    case Msg::Pen:
        if (onPen) onPen(p[0], p[1], f32At(p, 2), f32At(p, 6), f32At(p, 10));
        break;
    case Msg::Viewing:
        if (onViewing) onViewing(p[0] != 0);
        break;
    case Msg::ShareSize:
        if (onShareSize) onShareSize(int(u32At(p, 0)), int(u32At(p, 4)));
        break;
    case Msg::ShareConfig:
        if (onShareConfig) onShareConfig(p);
        break;
    case Msg::ShareFrame:
        if (onShareFrame) onShareFrame(Bytes(p.begin() + 1, p.end()));
        break;
    case Msg::ShareAudio:
        if (onShareAudio) onShareAudio(p);
        break;
    case Msg::MicAudio:
        if (onMicAudio) onMicAudio(p);
        break;
    case Msg::ShareStatus:
        if (onShareStatus) onShareStatus(p[0], p[1] != 0);
        break;
    case Msg::Ack:
        if (onAck) onAck(u32At(p, 0));
        break;
    case Msg::Hello:
        if (onHello) onHello(Hello::parse(p));
        break;
    default: // NOP, STANDBY (handled by the Wi-Fi link)
        break;
    }
}

} // namespace spanly
