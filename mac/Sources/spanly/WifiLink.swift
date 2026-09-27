import CryptoKit
import Foundation
import Network

/// Tablets over Wi-Fi: a Bonjour-advertised TCP listener (`_spanly._tcp`) whose traffic is
/// encrypted with keys derived from the secret paired over USB (WifiCrypto). Off unless "Allow
/// Wi-Fi Connection" is on. Each connection is its own [WifiConnection], so several tablets can
/// be connected at once.
final class WifiListener {
    static let port: UInt16 = 27184
    static let serviceType = "_spanly._tcp"

    /// A new connection, before anything is received on it: wire up its callbacks here.
    var onConnection: ((WifiConnection) -> Void)?

    private let queue = DispatchQueue(label: "wifi")
    private let lock = NSLock()
    private let secret: () -> Data
    private var listener: NWListener?
    private var connections: [WifiConnection] = []
    private var watchdog: DispatchSourceTimer?
    private let beacon = Beacon(name: Host.current().localizedName ?? "Mac")

    init(secret: @escaping () -> Data) {
        self.secret = secret
    }

    func setEnabled(_ enabled: Bool) {
        queue.async {
            if enabled { self.startListening() } else { self.stopListening() }
        }
    }

    private func startListening() {
        guard listener == nil else { return }
        let tcp = NWProtocolTCP.Options()
        tcp.noDelay = true
        let params = NWParameters(tls: nil, tcp: tcp)
        params.allowLocalEndpointReuse = true
        params.includePeerToPeer = true
        do {
            let l = try NWListener(using: params, on: NWEndpoint.Port(rawValue: WifiListener.port)!)
            l.service = NWListener.Service(name: Host.current().localizedName ?? "Mac", type: WifiListener.serviceType)
            l.newConnectionHandler = { [weak self] c in self?.accept(c) }
            l.stateUpdateHandler = { state in
                switch state {
                case let .failed(e): log("Wi-Fi listener failed: \(e)")
                case let .waiting(e): log("Wi-Fi listener waiting: \(e)")
                default: break
                }
            }
            l.serviceRegistrationUpdateHandler = { change in
                if case let .add(endpoint) = change { log("Wi-Fi: advertised as \(endpoint)") }
            }
            l.start(queue: queue)
            listener = l
            startWatchdog()
            beacon.start() // for tablets Bonjour doesn't reach (e.g. the Mac is on the tablet's hotspot)
            log("accepting paired tablets over Wi-Fi on port \(WifiListener.port)")
        } catch {
            log("could not listen for Wi-Fi connections: \(error)")
        }
    }

    private func stopListening() {
        listener?.cancel()
        listener = nil
        beacon.stop()
        watchdog?.cancel()
        watchdog = nil
        for c in lock.withLock({ connections }) { c.close(reason: "Wi-Fi turned off") }
    }

    /// Heartbeats both ways every 0.5 s (an idle screen sends no frames); 4 s of silence from a
    /// tablet means it's gone.
    private func startWatchdog() {
        let t = DispatchSource.makeTimerSource(queue: queue)
        t.schedule(deadline: .now() + 0.5, repeating: 0.5)
        t.setEventHandler { [weak self] in
            guard let self else { return }
            for c in self.lock.withLock({ self.connections }) {
                if c.silentFor > 4 {
                    c.close(reason: "no heartbeat")
                } else {
                    c.send(.nop, Data())
                }
            }
        }
        t.resume()
        watchdog = t
    }

    private func accept(_ nw: NWConnection) {
        let c = WifiConnection(nw, queue: queue, secret: secret())
        c.onClosed = { [weak self, weak c] in
            guard let self else { return }
            self.lock.withLock { self.connections.removeAll { $0 === c } }
        }
        lock.withLock { connections.append(c) }
        onConnection?(c)
        c.start()
    }
}

/// One tablet's encrypted Wi-Fi connection. Its state is touched only on the listener's queue,
/// except for the counters behind `lock`.
final class WifiConnection: Link {
    private let conn: NWConnection
    private let queue: DispatchQueue
    private let secret: Data
    private let lock = NSLock()
    private var keys: WifiCrypto.Keys?
    private var sendCounter: UInt64 = 0
    private var receiveCounter: UInt64 = 0
    private var buffer = Data()
    private var plain = Data()
    private var helloSeen = false
    private var established = false // HELLO or STANDBY: heartbeats may flow
    private var closed = false
    private var pending = 0
    private var lastReceive = Date()
    var onClosed: (() -> Void)?

    init(_ conn: NWConnection, queue: DispatchQueue, secret: Data) {
        self.conn = conn
        self.queue = queue
        self.secret = secret
        super.init()
    }

    override var isConnected: Bool { lock.withLock { helloSeen && !closed } }

    override var backlog: Int { lock.withLock { pending } }

    var silentFor: TimeInterval { Date().timeIntervalSince(lock.withLock { lastReceive }) }

    func start() {
        conn.stateUpdateHandler = { [weak self] state in
            switch state {
            case .failed, .cancelled: self?.close(reason: "connection ended")
            default: break
            }
        }
        conn.start(queue: queue)
        receive()
    }

    private func receive() {
        conn.receive(minimumIncompleteLength: 1, maximumLength: 1 << 16) { [weak self] data, _, done, error in
            guard let self else { return }
            if let data {
                self.lock.withLock { self.lastReceive = Date() }
                self.buffer.append(data)
                if !self.process() {
                    self.close(reason: "failed authentication")
                    return
                }
            }
            if done || error != nil {
                self.close(reason: "connection ended")
                return
            }
            self.receive()
        }
    }

    /// Handle the handshake and any complete records. False if the peer isn't paired.
    private func process() -> Bool {
        if keys == nil {
            let helloSize = WifiCrypto.magic.count + WifiCrypto.nonceSize
            guard buffer.count >= helloSize else { return true }
            guard buffer.prefix(WifiCrypto.magic.count) == WifiCrypto.magic else { return false }
            let clientNonce = Data(buffer[(buffer.startIndex + WifiCrypto.magic.count)..<(buffer.startIndex + helloSize)])
            buffer = Data(buffer.dropFirst(helloSize))
            let serverNonce = WifiCrypto.randomNonce()
            keys = WifiCrypto.serverKeys(secret: secret, clientNonce: clientNonce, serverNonce: serverNonce)
            conn.send(content: serverNonce, completion: .idempotent)
        }
        guard let keys else { return true }
        while buffer.count >= 4 {
            let len = Int(buffer.u32(at: 0))
            guard len <= Link.maxRecord else { return false }
            guard buffer.count >= 4 + len else { break }
            let record = Data(buffer[(buffer.startIndex + 4)..<(buffer.startIndex + 4 + len)])
            buffer = Data(buffer.dropFirst(4 + len))
            guard let p = try? WifiCrypto.open(record, key: keys.receive, counter: receiveCounter) else { return false }
            receiveCounter += 1
            plain.append(p)
        }
        consume(&plain)
        return true
    }

    override func handle(_ type: UInt8, _ p: Data) {
        if type == Msg.hello.rawValue, lock.withLock({ () -> Bool in
            defer { helloSeen = true; established = true }
            return !helloSeen
        }) {
            log("tablet connected over Wi-Fi")
            onClient?()
        } else if type == Msg.standby.rawValue, lock.withLock({ () -> Bool in
            defer { established = true }
            return !established
        }) {
            log("tablet keeps Wi-Fi ready while on USB")
        }
        super.handle(type, p)
    }

    override func send(_ type: Msg, _ payload: Data) {
        guard lock.withLock({ () -> Bool in
            guard established, !closed else { return false }
            pending += 1
            return true
        }) else { return }
        queue.async { [weak self] in
            guard let self, let keys = self.keys else { return }
            let sealed = WifiCrypto.seal(Link.encode(type, payload), key: keys.send, counter: self.sendCounter)
            self.sendCounter += 1
            var record = Data(capacity: sealed.count + 4)
            record.appendU32(UInt32(sealed.count))
            record.append(sealed)
            self.conn.send(content: record, completion: .contentProcessed { [weak self] _ in
                guard let self else { return }
                self.lock.withLock { self.pending -= 1 }
            })
        }
    }

    func close(reason: String) {
        let (first, wasUp) = lock.withLock { () -> (Bool, Bool) in
            defer { closed = true }
            return (!closed, helloSeen)
        }
        guard first else { return }
        conn.cancel()
        onClosed?()
        guard wasUp else { return }
        log("Wi-Fi link closed (\(reason))")
        onDisconnect?()
    }
}
