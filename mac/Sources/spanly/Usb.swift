// Raw USB transport using Android Open Accessory (AOA) mode.
//
// adb over USB tops out around 10-15 Mbit/s for video-shaped traffic on this class of
// tablet. In accessory mode the app gets the USB bulk endpoints directly, which runs at
// close to USB 2.0 speed with no per-packet round trips.
//
// Flow: find the chosen tablet (matched by USB serial number, so no other Android device
// is ever touched) -> ask it to re-enumerate as an accessory -> claim the accessory
// interface -> exchange the same length-prefixed messages as the TCP link.

import CLibUSB
import Foundation

/// An Android device on USB that could be used as the tablet.
struct UsbTablet: Equatable {
    let serial: String
    let name: String
    let accessoryMode: Bool
}

final class UsbLink: Link {
    static let manufacturer = "caigenix"
    static let model = "Spanly"

    private static let googleVendor: UInt16 = 0x18D1
    private static let accessoryProducts: ClosedRange<UInt16> = 0x2D00...0x2D05
    private static let appleVendor: UInt16 = 0x05AC
    /// Vendors that make Android phones/tablets (used together with interface sniffing).
    private static let androidVendors: Set<UInt16> = [
        0x18D1, 0x2717, 0x04E8, 0x22B8, 0x2A70, 0x12D1, 0x0BB4, 0x1004, 0x0FCE, 0x19D2, 0x2A45,
        0x05C6, 0x0E8D, 0x1BBB, 0x17EF, 0x0B05, 0x22D9, 0x2D95, 0x2AE5, 0x29A9, 0x1949, 0x2916,
    ]

    private var ctx: OpaquePointer?
    private let serial: () -> String?
    private let lock = NSLock()
    private var handle: OpaquePointer?
    private var epIn: UInt8 = 0
    private var epOut: UInt8 = 0
    private var maxPacket = 512
    private var helloSeen = false
    private var pending = 0
    private var lastSwitch: [String: Date] = [:]
    private var _connected: UsbTablet?

    /// The tablet the accessory link is open to (set once the app has said HELLO).
    var connected: UsbTablet? { lock.withLock { helloSeen ? _connected : nil } }
    private let writeQueue = DispatchQueue(label: "usb-write", qos: .userInteractive)
    private var heartbeat: DispatchSourceTimer?

    init?(serial: @escaping () -> String?) {
        self.serial = serial
        super.init()
        guard libusb_init(&ctx) == 0 else { return nil }
        let t = Thread { [weak self] in self?.scanLoop() }
        t.name = "usb-scan"
        t.start()
    }

    override var isConnected: Bool { lock.withLock { handle != nil && helloSeen } }

    override var backlog: Int { lock.withLock { pending } }

    // MARK: Discovery

    private func scanLoop() {
        while true {
            if lock.withLock({ handle == nil }) { scan() }
            Thread.sleep(forTimeInterval: 1)
        }
    }

    private func scan() {
        guard let want = serial() else { return }
        var list: UnsafeMutablePointer<OpaquePointer?>?
        let n = libusb_get_device_list(ctx, &list)
        guard n > 0, let list else { return }
        defer { libusb_free_device_list(list, 1) }

        for i in 0..<n {
            guard let dev = list[i] else { continue }
            var d = libusb_device_descriptor()
            guard libusb_get_device_descriptor(dev, &d) == 0,
                  serialNumber(dev, d) == want else { continue }
            if d.idVendor == UsbLink.googleVendor, UsbLink.accessoryProducts.contains(d.idProduct) {
                open(dev)
            } else {
                switchToAccessory(dev, serial: want)
            }
            return
        }
    }

    private func serialNumber(_ dev: OpaquePointer, _ d: libusb_device_descriptor) -> String? {
        guard d.iSerialNumber != 0 else { return nil }
        var h: OpaquePointer?
        guard libusb_open(dev, &h) == 0, let h else { return nil }
        defer { libusb_close(h) }
        return UsbLink.string(h, d.iSerialNumber)
    }

    private static func string(_ h: OpaquePointer, _ index: UInt8) -> String? {
        guard index != 0 else { return nil }
        var buf = [UInt8](repeating: 0, count: 256)
        let n = libusb_get_string_descriptor_ascii(h, index, &buf, Int32(buf.count))
        return n > 0 ? String(decoding: buf[0..<Int(n)], as: UTF8.self).trimmingCharacters(in: .whitespaces) : nil
    }

    private static func displayName(_ h: OpaquePointer, _ d: libusb_device_descriptor) -> String {
        let maker = string(h, d.iManufacturer) ?? ""
        let product = string(h, d.iProduct) ?? "Android device"
        return product.lowercased().hasPrefix(maker.lowercased()) || maker.isEmpty ? product : "\(maker) \(product)"
    }

    /// True for an Android phone/tablet: a known vendor, or an adb / MTP / PTP interface.
    private static func looksLikeAndroid(_ dev: OpaquePointer, _ d: libusb_device_descriptor) -> Bool {
        if d.idVendor == appleVendor { return false }
        if d.idVendor == googleVendor && accessoryProducts.contains(d.idProduct) { return true }
        if androidVendors.contains(d.idVendor) { return true }
        var cfg: UnsafeMutablePointer<libusb_config_descriptor>?
        guard libusb_get_active_config_descriptor(dev, &cfg) == 0, let cfg else { return false }
        defer { libusb_free_config_descriptor(cfg) }
        for i in 0..<Int(cfg.pointee.bNumInterfaces) {
            let alt = cfg.pointee.interface[i].altsetting[0]
            let adb = alt.bInterfaceClass == 0xFF && alt.bInterfaceSubClass == 0x42 && alt.bInterfaceProtocol == 0x01
            let imaging = alt.bInterfaceClass == 0x06 // PTP / MTP
            if adb || imaging { return true }
        }
        return false
    }

    /// Android devices currently plugged in, for the device menu.
    func tablets() -> [UsbTablet] {
        var list: UnsafeMutablePointer<OpaquePointer?>?
        let n = libusb_get_device_list(ctx, &list)
        guard n > 0, let list else { return [] }
        defer { libusb_free_device_list(list, 1) }
        var found: [UsbTablet] = []
        for i in 0..<n {
            guard let dev = list[i] else { continue }
            var d = libusb_device_descriptor()
            guard libusb_get_device_descriptor(dev, &d) == 0, d.iSerialNumber != 0,
                  UsbLink.looksLikeAndroid(dev, d) else { continue }
            var h: OpaquePointer?
            guard libusb_open(dev, &h) == 0, let h else {
                // Already claimed by us (the open accessory link): report what we know.
                if let c = lock.withLock({ _connected }) { found.append(c) }
                continue
            }
            defer { libusb_close(h) }
            guard let serial = UsbLink.string(h, d.iSerialNumber) else { continue }
            let accessory = d.idVendor == UsbLink.googleVendor && UsbLink.accessoryProducts.contains(d.idProduct)
            found.append(UsbTablet(serial: serial, name: UsbLink.displayName(h, d), accessoryMode: accessory))
        }
        var seen = Set<String>()
        return found.filter { seen.insert($0.serial).inserted }
    }

    /// AOA handshake: GET_PROTOCOL (51), SEND_STRING (52) x6, START (53).
    private func switchToAccessory(_ dev: OpaquePointer, serial: String) {
        if let last = lastSwitch[serial], Date().timeIntervalSince(last) < 10 { return } // still re-enumerating
        var h: OpaquePointer?
        guard libusb_open(dev, &h) == 0, let h else { return }
        defer { libusb_close(h) }

        var version = [UInt8](repeating: 0, count: 2)
        guard libusb_control_transfer(h, 0xC0, 51, 0, 0, &version, 2, 1000) == 2, version[0] >= 1 else {
            log("tablet does not support USB accessory mode; staying on adb")
            lastSwitch[serial] = .distantFuture
            return
        }
        let strings = [UsbLink.manufacturer, UsbLink.model, "Spanly second display", "1.0",
                       "https://github.com/caigenix/spanly", "spanly"]
        for (i, str) in strings.enumerated() {
            var bytes = Array(str.utf8) + [0]
            _ = libusb_control_transfer(h, 0x40, 52, 0, UInt16(i), &bytes, UInt16(bytes.count), 1000)
        }
        _ = libusb_control_transfer(h, 0x40, 53, 0, 0, nil, 0, 1000)
        lastSwitch[serial] = Date()
        log("asked the tablet to switch to USB accessory mode")
    }

    // MARK: Session

    private func open(_ dev: OpaquePointer) {
        var h: OpaquePointer?
        guard libusb_open(dev, &h) == 0, let h else { return }
        var desc = libusb_device_descriptor()
        _ = libusb_get_device_descriptor(dev, &desc)
        let info = UsbTablet(serial: UsbLink.string(h, desc.iSerialNumber) ?? "",
                             name: UsbLink.displayName(h, desc), accessoryMode: true)

        // Interface 0 is the accessory interface (interface 1, if present, is adb).
        var cfg: UnsafeMutablePointer<libusb_config_descriptor>?
        guard libusb_get_active_config_descriptor(dev, &cfg) == 0, let cfg else {
            libusb_close(h)
            return
        }
        let alt = cfg.pointee.interface[0].altsetting[0]
        var inEp: UInt8 = 0, outEp: UInt8 = 0, packet = 512
        for e in 0..<Int(alt.bNumEndpoints) {
            let ep = alt.endpoint[e]
            guard ep.bmAttributes & 0x3 == 2 else { continue } // bulk
            if ep.bEndpointAddress & 0x80 != 0 {
                inEp = ep.bEndpointAddress
            } else {
                outEp = ep.bEndpointAddress
                packet = Int(ep.wMaxPacketSize)
            }
        }
        libusb_free_config_descriptor(cfg)
        guard inEp != 0, outEp != 0, libusb_claim_interface(h, 0) == 0 else {
            libusb_close(h)
            return
        }

        lock.withLock {
            handle = h
            _connected = info
            epIn = inEp
            epOut = outEp
            maxPacket = max(packet, 64)
            helloSeen = false
            pending = 0
        }
        let t = Thread { [weak self] in self?.readLoop(h, inEp) }
        t.name = "usb-read"
        t.qualityOfService = .userInteractive
        t.start()
        startHeartbeat()
    }

    /// The app sends a heartbeat every 0.5 s, so a read only times out when it is gone.
    /// (Frequent timeouts are also worth avoiding: libusb on macOS aborts the pipe on each one.)
    private func readLoop(_ h: OpaquePointer, _ ep: UInt8) {
        var buf = Data()
        var chunk = [UInt8](repeating: 0, count: 16384)
        while lock.withLock({ handle == h }) {
            var got: Int32 = 0
            let r = libusb_bulk_transfer(h, ep, &chunk, Int32(chunk.count), &got, 3000)
            if got > 0 {
                buf.append(chunk, count: Int(got))
                consume(&buf)
            }
            if r == LIBUSB_ERROR_TIMEOUT.rawValue {
                if lock.withLock({ helloSeen }) {
                    close(h, reason: "no heartbeat from the app")
                    return
                }
            } else if r != 0 {
                close(h, reason: String(cString: libusb_error_name(r)))
                return
            }
        }
    }

    override func handle(_ type: UInt8, _ p: Data) {
        if type == Msg.hello.rawValue {
            let first = lock.withLock { () -> Bool in
                defer { helloSeen = true }
                return !helloSeen
            }
            if first {
                log("tablet connected over USB accessory")
                onClient?()
            }
        }
        super.handle(type, p)
    }

    /// Until the app says HELLO, keep asking for it (it may still hold a session from before
    /// the link was reopened). After that, a NOP keeps the tablet's blocking read responsive
    /// and a write that stops being accepted tells us the app went away.
    private func startHeartbeat() {
        let t = DispatchSource.makeTimerSource(queue: .global(qos: .utility))
        t.schedule(deadline: .now() + 0.2, repeating: 0.5)
        t.setEventHandler { [weak self] in
            guard let self, self.lock.withLock({ self.pending == 0 }) else { return }
            self.send(self.lock.withLock { self.helloSeen } ? .nop : .helloRequest, Data())
        }
        t.resume()
        lock.withLock {
            heartbeat?.cancel()
            heartbeat = t
        }
    }

    override func send(_ type: Msg, _ payload: Data) {
        guard let (h, ep, packet) = lock.withLock({ () -> (OpaquePointer, UInt8, Int)? in
            guard let handle, helloSeen || type == .nop || type == .helloRequest else { return nil }
            pending += 1
            return (handle, epOut, maxPacket)
        }) else { return }
        var d = Link.encode(type, payload)
        // A transfer that is an exact multiple of the packet size would need a zero-length
        // packet to complete on the tablet; pad with a NOP message instead.
        if d.count % packet == 0 { d.append(Link.encode(.nop, Data())) }
        writeQueue.async { self.write(d, h, ep) }
    }

    private func write(_ data: Data, _ h: OpaquePointer, _ ep: UInt8) {
        defer { lock.withLock { pending = max(0, pending - 1) } }
        guard lock.withLock({ handle == h }) else { return }
        var bytes = [UInt8](data)
        var off = 0
        while off < bytes.count {
            var sent: Int32 = 0
            let r = bytes.withUnsafeMutableBufferPointer {
                libusb_bulk_transfer(h, ep, $0.baseAddress! + off, Int32($0.count - off), &sent, 1000)
            }
            off += Int(sent)
            if r != 0 {
                close(h, reason: r == LIBUSB_ERROR_TIMEOUT.rawValue ? "app stopped reading" : String(cString: libusb_error_name(r)))
                return
            }
        }
    }

    private func close(_ h: OpaquePointer, reason: String) {
        let wasConnected = lock.withLock { () -> Bool? in
            guard handle == h else { return nil }
            let was = helloSeen
            handle = nil
            helloSeen = false
            pending = 0
            heartbeat?.cancel()
            heartbeat = nil
            return was
        }
        guard let wasConnected else { return }
        libusb_release_interface(h, 0)
        libusb_close(h)
        // Before HELLO this just means the app isn't running yet; retry quietly.
        if wasConnected {
            log("USB accessory link closed (\(reason))")
            onDisconnect?()
        }
    }
}
