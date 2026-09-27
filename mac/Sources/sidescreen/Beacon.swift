import Darwin
import Foundation

/// Announces this Mac with a small UDP broadcast every second on each network it is on, so a
/// tablet finds it where Bonjour doesn't reach, notably when the Mac has joined the tablet's own
/// hotspot (no router). Payload: "SSB1" + the Mac's name, which the tablet knows from pairing.
/// Runs while Wi-Fi connections are allowed.
final class Beacon {
    static let magic = Data("SSB1".utf8)

    private let queue = DispatchQueue(label: "beacon")
    private let payload: Data
    private var fd: Int32 = -1
    private var fd6: Int32 = -1
    private var timer: DispatchSourceTimer?

    init(name: String) {
        payload = Beacon.magic + Data(name.utf8.prefix(200))
    }

    func start() {
        queue.async {
            guard self.timer == nil else { return }
            self.fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)
            guard self.fd >= 0 else { return log("beacon: no socket") }
            var on: Int32 = 1
            setsockopt(self.fd, SOL_SOCKET, SO_BROADCAST, &on, socklen_t(MemoryLayout<Int32>.size))
            self.fd6 = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP) // IPv6-only networks (some phone hotspots)
            let t = DispatchSource.makeTimerSource(queue: self.queue)
            t.schedule(deadline: .now(), repeating: 1)
            t.setEventHandler { [weak self] in self?.announce() }
            t.resume()
            self.timer = t
        }
    }

    func stop() {
        queue.async {
            self.timer?.cancel()
            self.timer = nil
            if self.fd >= 0 { close(self.fd) }
            if self.fd6 >= 0 { close(self.fd6) }
            self.fd = -1
            self.fd6 = -1
        }
    }

    /// One broadcast per IPv4 network and one all-nodes multicast (ff02::1) per IPv6 one, on
    /// every network, not just the default one: a Mac can be on Ethernet and on a tablet's
    /// hotspot at the same time.
    private func announce() {
        var list: UnsafeMutablePointer<ifaddrs>?
        guard getifaddrs(&list) == 0, let first = list else { return }
        defer { freeifaddrs(list) }
        var v6Done = Set<UInt32>()
        for ifa in sequence(first: first, next: { $0.pointee.ifa_next }) {
            let flags = Int32(ifa.pointee.ifa_flags)
            if flags & IFF_UP != 0, flags & IFF_MULTICAST != 0, flags & IFF_LOOPBACK == 0,
               ifa.pointee.ifa_addr?.pointee.sa_family == UInt8(AF_INET6),
               case let index = if_nametoindex(ifa.pointee.ifa_name), index != 0, v6Done.insert(index).inserted {
                announceV6(on: index)
            }
            guard flags & IFF_UP != 0, flags & IFF_BROADCAST != 0, flags & IFF_LOOPBACK == 0,
                  let addr = ifa.pointee.ifa_addr, addr.pointee.sa_family == UInt8(AF_INET),
                  var dest = ifa.pointee.ifa_dstaddr?.withMemoryRebound(to: sockaddr_in.self, capacity: 1, { $0.pointee })
            else { continue }
            dest.sin_port = UInt16(WifiListener.port).bigEndian
            payload.withUnsafeBytes { bytes in
                withUnsafePointer(to: &dest) { d in
                    d.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                        _ = sendto(fd, bytes.baseAddress, bytes.count, 0, $0, socklen_t(MemoryLayout<sockaddr_in>.size))
                    }
                }
            }
        }
    }

    private func announceV6(on index: UInt32) {
        guard fd6 >= 0 else { return }
        var dest = sockaddr_in6()
        dest.sin6_len = UInt8(MemoryLayout<sockaddr_in6>.size)
        dest.sin6_family = sa_family_t(AF_INET6)
        dest.sin6_port = UInt16(WifiListener.port).bigEndian
        inet_pton(AF_INET6, "ff02::1", &dest.sin6_addr)
        dest.sin6_scope_id = index
        payload.withUnsafeBytes { bytes in
            withUnsafePointer(to: &dest) { d in
                d.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                    _ = sendto(fd6, bytes.baseAddress, bytes.count, 0, $0, socklen_t(MemoryLayout<sockaddr_in6>.size))
                }
            }
        }
    }
}
