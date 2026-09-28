// UsbLink's session: reading, writing and heartbeats on an open accessory (see usb.hpp).
#include "core/log.hpp"
#include "core/usb.hpp"

#include <libusb.h>

namespace spanly {

using namespace std::chrono_literals;

void UsbLink::handle(const Message& m) {
    if (m.type == uint8_t(Msg::Hello)) {
        bool first;
        {
            std::scoped_lock l(m_);
            first = !helloSeen_;
            helloSeen_ = true;
        }
        if (first) log("tablet connected over USB accessory");
    }
    Link::handle(m);
}

/// The app sends a heartbeat every 0.5 s, so a read only times out when it is gone. (Frequent
/// timeouts are also worth avoiding: libusb on macOS aborts the pipe on each one.)
void UsbLink::readLoop(libusb_device_handle* h, uint8_t ep) {
    Reader reader;
    std::vector<unsigned char> chunk(16384);
    while (true) {
        {
            std::scoped_lock l(m_);
            if (handle_ != h) return;
        }
        int got = 0;
        int r = libusb_bulk_transfer(h, ep, chunk.data(), int(chunk.size()), &got, 3000);
        {
            // Closed while this read was waiting: whatever arrived belongs to the next session.
            std::scoped_lock l(m_);
            if (handle_ != h) return;
        }
        if (got > 0) {
            reader.push(ByteView(chunk.data(), size_t(got)));
            drain(reader);
        }
        if (r == LIBUSB_ERROR_TIMEOUT) {
            std::scoped_lock l(m_);
            if (!helloSeen_) continue; // the app isn't running yet
        } else if (r == 0) {
            continue;
        }
        return close(h, r == LIBUSB_ERROR_TIMEOUT ? "no heartbeat from the app" : libusb_error_name(r));
    }
}

void UsbLink::send(Msg type, ByteView payload) {
    {
        std::scoped_lock l(m_);
        if (!handle_ || !helloSeen_) return;
    }
    queue(type, payload);
}

void UsbLink::queue(Msg type, ByteView payload) {
    std::scoped_lock l(m_);
    if (!handle_) return;
    queue_.push_back(encode(type, payload));
    ++pending_;
    wake_.notify_one();
}

void UsbLink::writeLoop(libusb_device_handle* h, uint8_t ep, size_t packet) {
    while (true) {
        Bytes d;
        {
            std::unique_lock l(m_);
            wake_.wait(l, [&] { return handle_ != h || !queue_.empty(); });
            if (handle_ != h) return;
            d = std::move(queue_.front());
            queue_.pop_front();
        }
        // A transfer that is an exact multiple of the packet size would need a zero-length
        // packet to complete on the tablet; pad with a NOP message instead.
        if (d.size() % packet == 0) {
            Bytes nop = encode(Msg::Nop);
            d.insert(d.end(), nop.begin(), nop.end());
        }
        size_t off = 0;
        int r = 0;
        while (off < d.size() && r == 0) {
            int sent = 0;
            r = libusb_bulk_transfer(h, ep, d.data() + off, int(d.size() - off), &sent, 1000);
            off += size_t(sent);
        }
        {
            std::scoped_lock l(m_);
            if (pending_ > 0) --pending_;
        }
        if (r != 0) return close(h, r == LIBUSB_ERROR_TIMEOUT ? "app stopped reading" : libusb_error_name(r));
    }
}

/// Until the app says HELLO, keep asking for it (it may still hold a session from before the
/// link reopened). After that a NOP keeps the tablet's blocking read responsive, and a write
/// that stops being accepted tells us the app went away.
void UsbLink::heartbeatLoop(libusb_device_handle* h) {
    while (true) {
        std::this_thread::sleep_for(500ms);
        bool hello;
        {
            std::scoped_lock l(m_);
            if (handle_ != h) return;
            if (pending_ > 0) continue;
            hello = helloSeen_;
        }
        queue(hello ? Msg::Nop : Msg::HelloRequest, {});
    }
}

void UsbLink::close(libusb_device_handle* h, const std::string& reason) {
    bool wasConnected;
    {
        std::scoped_lock l(m_);
        if (handle_ != h) return;
        wasConnected = helloSeen_;
        if (wasConnected && open_) silentOpens_.erase(open_->serial);
        handle_ = nullptr;
        helloSeen_ = false;
        queue_.clear();
        pending_ = 0;
        wake_.notify_all();
    }
    // Before HELLO this just means the app isn't running yet; retry quietly (and count it).
    if (!wasConnected) {
        std::scoped_lock l(m_);
        if (open_) ++silentOpens_[open_->serial];
        return;
    }
    log("USB accessory link closed ({})", reason);
    if (onDisconnect) onDisconnect();
}

} // namespace spanly
