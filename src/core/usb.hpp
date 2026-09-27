#pragma once
// Raw USB to the tablet in Android Open Accessory (AOA) mode: the fast path (close to USB 2.0
// speed, no per-packet round trips). The chosen tablet (matched by USB serial number, so no
// other Android device is ever touched) is asked to re-enumerate as an accessory; then messages
// go over its bulk endpoints.
//
// Linux needs read/write access to the device (packaging/linux/70-spanly.rules). Windows needs
// the WinUSB driver on the accessory interface.

#include "core/link.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <thread>

struct libusb_context;
struct libusb_device;
struct libusb_device_handle;

namespace spanly {

/// An Android device on USB that could be used as the tablet.
struct UsbTablet {
    std::string serial;
    std::string name;
    bool accessoryMode = false;
};

class UsbLink : public Link {
public:
    static constexpr const char* kManufacturer = "caigenix";
    static constexpr const char* kModel = "Spanly";

    /// `serial` names the tablet to use (the chosen one); empty: none.
    static std::shared_ptr<UsbLink> create(std::function<std::string()> serial);
    ~UsbLink() override;

    Kind kind() const override { return Kind::Usb; }
    bool connected() const override;
    void send(Msg type, ByteView payload = {}) override;
    size_t backlog() const override;

    /// The tablet the accessory link is open to (once its app has said HELLO).
    std::optional<UsbTablet> tablet() const;
    /// Android devices currently plugged in, for the device menu.
    std::vector<UsbTablet> tablets();

protected:
    void handle(const Message& m) override;

private:
    explicit UsbLink(std::function<std::string()> serial) : serial_(std::move(serial)) {}
    void scanLoop();
    void scan();
    void switchToAccessory(libusb_device* dev, const std::string& serial);
    void open(libusb_device* dev);
    void readLoop(libusb_device_handle* h, uint8_t ep);
    void writeLoop(libusb_device_handle* h, uint8_t ep, size_t packet);
    void heartbeatLoop(libusb_device_handle* h);
    void close(libusb_device_handle* h, const std::string& reason);
    void queue(Msg type, ByteView payload);

    libusb_context* ctx_ = nullptr;
    std::function<std::string()> serial_;
    std::atomic<bool> running_{true};
    std::thread scanner_;
    std::map<std::string, std::chrono::steady_clock::time_point> lastSwitch_; // scanner thread only

    mutable std::mutex m_;
    std::condition_variable wake_;
    std::deque<Bytes> queue_;
    size_t pending_ = 0;
    libusb_device_handle* handle_ = nullptr;
    bool helloSeen_ = false;
    std::optional<UsbTablet> open_;
    /// Accessory sessions in a row that ended without a HELLO, per tablet: a tablet left in
    /// accessory mode by an older version (other identity strings) ignores us.
    std::map<std::string, int> silentOpens_;
    std::map<std::string, std::chrono::steady_clock::time_point> lastRedo_; // scanner thread only
};

} // namespace spanly
