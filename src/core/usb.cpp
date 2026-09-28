#include "core/usb.hpp"

#include "core/log.hpp"

#include <libusb.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <set>

namespace spanly {

using namespace std::chrono_literals;

namespace {

constexpr uint16_t kGoogle = 0x18D1;
constexpr uint16_t kApple = 0x05AC;
// Vendors that make Android phones/tablets (used together with interface sniffing).
constexpr std::array<uint16_t, 22> kAndroidVendors = {0x18D1, 0x2717, 0x04E8, 0x22B8, 0x2A70, 0x12D1, 0x0BB4, 0x1004,
                                                      0x0FCE, 0x19D2, 0x2A45, 0x05C6, 0x0E8D, 0x1BBB, 0x17EF, 0x0B05,
                                                      0x22D9, 0x2D95, 0x2AE5, 0x29A9, 0x1949, 0x2916};

bool isAccessory(const libusb_device_descriptor& d) {
    return d.idVendor == kGoogle && d.idProduct >= 0x2D00 && d.idProduct <= 0x2D05;
}

std::string text(libusb_device_handle* h, uint8_t index) {
    if (index == 0) return {};
    unsigned char buf[256];
    int n = libusb_get_string_descriptor_ascii(h, index, buf, sizeof buf);
    if (n <= 0) return {};
    std::string s(reinterpret_cast<char*>(buf), size_t(n));
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
        s.pop_back();
    return s;
}

std::string lower(std::string s) {
    for (auto& c : s)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string displayName(libusb_device_handle* h, const libusb_device_descriptor& d) {
    std::string maker = text(h, d.iManufacturer), product = text(h, d.iProduct);
    if (product.empty()) product = "Android device";
    return maker.empty() || lower(product).starts_with(lower(maker)) ? product : maker + " " + product;
}

/// A known Android vendor, or an adb / MTP / PTP interface.
bool looksLikeAndroid(libusb_device* dev, const libusb_device_descriptor& d) {
    if (d.idVendor == kApple) return false;
    if (isAccessory(d) || std::ranges::find(kAndroidVendors, d.idVendor) != kAndroidVendors.end()) return true;
    libusb_config_descriptor* cfg = nullptr;
    if (libusb_get_active_config_descriptor(dev, &cfg) != 0) return false;
    bool found = false;
    for (unsigned i = 0; i < cfg->bNumInterfaces && !found; ++i) {
        const auto& alt = cfg->interface[i].altsetting[0];
        bool adb = alt.bInterfaceClass == 0xFF && alt.bInterfaceSubClass == 0x42 && alt.bInterfaceProtocol == 0x01;
        found = adb || alt.bInterfaceClass == 0x06; // PTP / MTP
    }
    libusb_free_config_descriptor(cfg);
    return found;
}

} // namespace

std::shared_ptr<UsbLink> UsbLink::create(std::function<std::string()> serial) {
    auto link = std::shared_ptr<UsbLink>(new UsbLink(std::move(serial)));
    if (libusb_init(&link->ctx_) != 0) {
        log("libusb unavailable; no raw USB link");
        return nullptr;
    }
    link->scanner_ = std::thread([link = link.get()] { link->scanLoop(); });
    return link;
}

UsbLink::~UsbLink() {
    running_ = false;
    if (scanner_.joinable()) scanner_.join();
}

bool UsbLink::connected() const {
    std::scoped_lock l(m_);
    return handle_ && helloSeen_;
}

size_t UsbLink::backlog() const {
    std::scoped_lock l(m_);
    return pending_;
}

std::optional<UsbTablet> UsbLink::tablet() const {
    std::scoped_lock l(m_);
    return helloSeen_ ? open_ : std::nullopt;
}

void UsbLink::scanLoop() {
    while (running_) {
        bool idle;
        {
            std::scoped_lock l(m_);
            idle = !handle_ && !open_;
        }
        if (idle) scan();
        std::this_thread::sleep_for(1s);
    }
}

void UsbLink::scan() {
    std::string want = serial_ ? serial_() : std::string();
    if (want.empty()) return;
    libusb_device** list = nullptr;
    ssize_t n = libusb_get_device_list(ctx_, &list);
    for (ssize_t i = 0; i < n; ++i) {
        libusb_device_descriptor d{};
        libusb_device_handle* h = nullptr;
        if (libusb_get_device_descriptor(list[i], &d) != 0 || d.iSerialNumber == 0 || libusb_open(list[i], &h) != 0)
            continue;
        std::string serial = text(h, d.iSerialNumber);
        libusb_close(h);
        if (serial != want) continue;
        bool redo = false;
        if (isAccessory(d)) {
            std::scoped_lock l(m_);
            if (silentOpens_[serial] >= 2 && !answered_.contains(serial) && reset_.insert(serial).second) {
                redo = true;
                silentOpens_[serial] = 0;
                lastSwitch_.erase(serial);
            }
        }
        if (redo) {
            // Android keeps the accessory it has until it sees a disconnect: reset the port, and
            // set accessory mode up again once the tablet is back in its normal mode.
            log("the tablet doesn't answer on its accessory link; resetting it");
            libusb_device_handle* r = nullptr;
            if (libusb_open(list[i], &r) == 0) {
                libusb_reset_device(r);
                libusb_close(r);
            }
        } else if (isAccessory(d)) {
            open(list[i]);
        } else {
            switchToAccessory(list[i], want);
        }
        break;
    }
    if (list) libusb_free_device_list(list, 1);
}

std::vector<UsbTablet> UsbLink::tablets() {
    std::vector<UsbTablet> found;
    std::set<std::string> seen;
    libusb_device** list = nullptr;
    ssize_t n = libusb_get_device_list(ctx_, &list);
    for (ssize_t i = 0; i < n; ++i) {
        libusb_device_descriptor d{};
        if (libusb_get_device_descriptor(list[i], &d) != 0 || d.iSerialNumber == 0 || !looksLikeAndroid(list[i], d))
            continue;
        libusb_device_handle* h = nullptr;
        if (libusb_open(list[i], &h) != 0) {
            // Already claimed by us (the open accessory link): report what we know.
            std::scoped_lock l(m_);
            if (open_ && seen.insert(open_->serial).second) found.push_back(*open_);
            continue;
        }
        UsbTablet t{text(h, d.iSerialNumber), displayName(h, d), isAccessory(d)};
        libusb_close(h);
        if (!t.serial.empty() && seen.insert(t.serial).second) found.push_back(t);
    }
    if (list) libusb_free_device_list(list, 1);
    return found;
}

/// AOA handshake: GET_PROTOCOL (51), SEND_STRING (52) x6, START (53).
void UsbLink::switchToAccessory(libusb_device* dev, const std::string& serial) {
    auto last = lastSwitch_.find(serial);
    if (last != lastSwitch_.end() && std::chrono::steady_clock::now() < last->second + 10s) return; // re-enumerating
    libusb_device_handle* h = nullptr;
    if (libusb_open(dev, &h) != 0) return;
    unsigned char version[2] = {};
    if (libusb_control_transfer(h, 0xC0, 51, 0, 0, version, 2, 1000) != 2 || version[0] < 1) {
        log("tablet does not support USB accessory mode");
        lastSwitch_[serial] = std::chrono::steady_clock::now() + 24h;
        libusb_close(h);
        return;
    }
    const std::array<std::string, 6> strings = {
        kManufacturer, kModel, "Spanly second display", "1.0", "https://github.com/caigenix/spanly", "spanly"};
    for (size_t i = 0; i < strings.size(); ++i) {
        std::string s = strings[i];
        libusb_control_transfer(h, 0x40, 52, 0, uint16_t(i), reinterpret_cast<unsigned char*>(s.data()),
                                uint16_t(s.size() + 1), 1000);
    }
    libusb_control_transfer(h, 0x40, 53, 0, 0, nullptr, 0, 1000);
    libusb_close(h);
    lastSwitch_[serial] = std::chrono::steady_clock::now();
    log("asked the tablet to switch to USB accessory mode");
}

void UsbLink::open(libusb_device* dev) {
    libusb_device_handle* h = nullptr;
    if (libusb_open(dev, &h) != 0) return;
    libusb_device_descriptor d{};
    libusb_get_device_descriptor(dev, &d);
    UsbTablet info{text(h, d.iSerialNumber), displayName(h, d), true};

    // Interface 0 is the accessory interface (interface 1, if present, is adb).
    libusb_config_descriptor* cfg = nullptr;
    uint8_t in = 0, out = 0;
    size_t packet = 512;
    if (libusb_get_active_config_descriptor(dev, &cfg) == 0) {
        const auto& alt = cfg->interface[0].altsetting[0];
        for (unsigned e = 0; e < alt.bNumEndpoints; ++e) {
            const auto& ep = alt.endpoint[e];
            if ((ep.bmAttributes & 3) != LIBUSB_TRANSFER_TYPE_BULK) continue;
            if (ep.bEndpointAddress & LIBUSB_ENDPOINT_IN) {
                in = ep.bEndpointAddress;
            } else {
                out = ep.bEndpointAddress;
                packet = std::max<size_t>(ep.wMaxPacketSize, 64);
            }
        }
        libusb_free_config_descriptor(cfg);
    }
    libusb_set_auto_detach_kernel_driver(h, 1); // Linux only; harmless elsewhere
    int claimed = in && out ? libusb_claim_interface(h, 0) : LIBUSB_ERROR_NOT_FOUND;
    if (claimed != 0) {
        log("could not claim the tablet's accessory interface: {}{}", libusb_error_name(claimed),
#ifdef _WIN32
            " (install the WinUSB driver for it)");
#else
            "");
#endif
        libusb_close(h);
        return;
    }
    {
        std::scoped_lock l(m_);
        handle_ = h;
        open_ = info;
        helloSeen_ = false;
        queue_.clear();
        pending_ = 0;
    }
    // The reader owns the handle: it starts the writer and heartbeat, and cleans up after both.
    std::thread([this, h, in, out, packet] {
        std::thread writer([=, this] { writeLoop(h, out, packet); });
        std::thread heartbeat([=, this] { heartbeatLoop(h); });
        readLoop(h, in);
        writer.join();
        heartbeat.join();
        libusb_release_interface(h, 0);
        libusb_close(h);
        std::scoped_lock l(m_);
        open_.reset(); // the scanner may open the tablet again
    }).detach();
}

} // namespace spanly
