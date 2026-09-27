#pragma once
// A connection to a tablet: raw USB accessory (UsbLink), TCP through adb or encrypted Wi-Fi
// (TcpLink). All carry the same messages; received ones arrive through the callbacks below,
// on the link's own thread.

#include "core/protocol.hpp"

#include <functional>
#include <memory>
#include <mutex>
#include <string>

namespace spanly {

class TabletSession;

class Link : public std::enable_shared_from_this<Link> {
public:
    enum class Kind { None, Usb, Wifi, Adb };

    virtual ~Link() = default;
    virtual Kind kind() const { return Kind::None; }
    /// The tablet's app has said HELLO on it and it is still open.
    virtual bool connected() const { return false; }
    virtual void send(Msg, ByteView = {}) {}
    /// Messages handed to the link but not yet written out.
    virtual size_t backlog() const { return 0; }

    static std::shared_ptr<Link> none();
    static const char* kindName(Kind k);

    // Received messages.
    std::function<void(const Hello&)> onHello;
    std::function<void()> onDisconnect;
    std::function<void(uint32_t)> onAck;
    std::function<void(uint8_t action, float x, float y)> onTouch;
    /// (kind, phase, last, x, y, dx, dy)
    std::function<void(uint8_t, uint8_t, bool, float, float, float, float)> onScroll;
    std::function<void(int8_t direction, float x, float y)> onZoom;
    /// (action, buttons, x, y, pressure)
    std::function<void(uint8_t, uint8_t, float, float, float)> onPen;
    std::function<void(bool)> onViewing;
    std::function<void(int w, int h)> onShareSize;
    std::function<void(const Bytes&)> onShareConfig;
    std::function<void(const Bytes&)> onShareFrame; // flags byte stripped
    std::function<void(uint8_t state, bool control)> onShareStatus;
    std::function<void(const Bytes&)> onShareAudio;
    std::function<void(const Bytes&)> onMicAudio;

    /// The tablet this connection belongs to (known from its HELLO).
    std::shared_ptr<TabletSession> session() const;
    void setSession(const std::shared_ptr<TabletSession>& s);

protected:
    /// Dispatch every complete message in `reader` to the callbacks.
    void drain(Reader& reader);
    virtual void handle(const Message& m);

private:
    mutable std::mutex sessionLock_;
    std::weak_ptr<TabletSession> session_;
};

} // namespace spanly
