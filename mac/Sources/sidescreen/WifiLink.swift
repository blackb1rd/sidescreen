import CryptoKit
import Foundation
import Network

/// The tablet over Wi-Fi: a Bonjour-advertised TCP listener (`_sidescreen._tcp`) whose
/// traffic is encrypted with keys derived from the secret paired over USB (WifiCrypto).
/// Off unless "Allow Wi-Fi Connection" is on. One tablet at a time.
final class WifiLink: Link {
    static let port: UInt16 = 27184
    static let serviceType = "_sidescreen._tcp"

    private let queue = DispatchQueue(label: "wifi")
    private let lock = NSLock()
    private let secret: () -> Data
    private var listener: NWListener?
    private var session: Session?
    private var watchdog: DispatchSourceTimer?

    /// One connection's handshake and crypto state. Touched only on `queue`.
    private final class Session {
        let conn: NWConnection
        var keys: WifiCrypto.Keys?
        var sendCounter: UInt64 = 0
        var receiveCounter: UInt64 = 0
        var buffer = Data()
        var plain = Data()
        var helloSeen = false
        var pending = 0
        var lastReceive = Date()

        init(_ conn: NWConnection) { self.conn = conn }
    }

    init(secret: @escaping () -> Data) {
        self.secret = secret
        super.init()
    }

    override var isConnected: Bool { lock.withLock { session?.helloSeen == true } }

    override var backlog: Int { lock.withLock { session?.pending ?? 0 } }

    // MARK: Listening

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
            let l = try NWListener(using: params, on: NWEndpoint.Port(rawValue: WifiLink.port)!)
            l.service = NWListener.Service(name: Host.current().localizedName ?? "Mac", type: WifiLink.serviceType)
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
            log("accepting paired tablets over Wi-Fi on port \(WifiLink.port)")
        } catch {
            log("could not listen for Wi-Fi connections: \(error)")
        }
    }

    private func stopListening() {
        listener?.cancel()
        listener = nil
        watchdog?.cancel()
        watchdog = nil
        if let s = lock.withLock({ session }) { close(s, reason: "Wi-Fi turned off") }
    }

    /// The tablet sends a heartbeat every 0.5 s; 4 s of silence means it's gone.
    private func startWatchdog() {
        let t = DispatchSource.makeTimerSource(queue: queue)
        t.schedule(deadline: .now() + 1, repeating: 1)
        t.setEventHandler { [weak self] in
            guard let self, let s = self.lock.withLock({ self.session }),
                  Date().timeIntervalSince(s.lastReceive) > 4 else { return }
            self.close(s, reason: "no heartbeat")
        }
        t.resume()
        watchdog = t
    }

    // MARK: Session

    private func accept(_ c: NWConnection) {
        let s = Session(c)
        let old = lock.withLock { () -> Session? in
            defer { session = s }
            return session
        }
        if let old { close(old, reason: "replaced by a new connection") }
        c.stateUpdateHandler = { [weak self] state in
            switch state {
            case .failed, .cancelled: self?.close(s, reason: "connection ended")
            default: break
            }
        }
        c.start(queue: queue)
        receive(s)
    }

    private func receive(_ s: Session) {
        s.conn.receive(minimumIncompleteLength: 1, maximumLength: 1 << 16) { [weak self] data, _, done, error in
            guard let self else { return }
            if let data {
                s.lastReceive = Date()
                s.buffer.append(data)
                if !self.process(s) {
                    self.close(s, reason: "failed authentication")
                    return
                }
            }
            if done || error != nil {
                self.close(s, reason: "connection ended")
                return
            }
            self.receive(s)
        }
    }

    /// Handle the handshake and any complete records. False if the peer isn't paired.
    private func process(_ s: Session) -> Bool {
        if s.keys == nil {
            let helloSize = WifiCrypto.magic.count + WifiCrypto.nonceSize
            guard s.buffer.count >= helloSize else { return true }
            guard s.buffer.prefix(WifiCrypto.magic.count) == WifiCrypto.magic else { return false }
            let clientNonce = Data(s.buffer[(s.buffer.startIndex + WifiCrypto.magic.count)..<(s.buffer.startIndex + helloSize)])
            s.buffer = Data(s.buffer.dropFirst(helloSize))
            let serverNonce = WifiCrypto.randomNonce()
            s.keys = WifiCrypto.serverKeys(secret: secret(), clientNonce: clientNonce, serverNonce: serverNonce)
            s.conn.send(content: serverNonce, completion: .idempotent)
        }
        guard let keys = s.keys else { return true }
        while s.buffer.count >= 4 {
            let len = Int(s.buffer.u32(at: 0))
            guard len <= Link.maxRecord else { return false }
            guard s.buffer.count >= 4 + len else { break }
            let record = Data(s.buffer[(s.buffer.startIndex + 4)..<(s.buffer.startIndex + 4 + len)])
            s.buffer = Data(s.buffer.dropFirst(4 + len))
            guard let plain = try? WifiCrypto.open(record, key: keys.receive, counter: s.receiveCounter) else { return false }
            s.receiveCounter += 1
            s.plain.append(plain)
        }
        consume(&s.plain)
        return true
    }

    override func handle(_ type: UInt8, _ p: Data) {
        if type == Msg.hello.rawValue, let s = lock.withLock({ session }), !s.helloSeen {
            lock.withLock { s.helloSeen = true }
            log("tablet connected over Wi-Fi")
            onClient?()
        }
        super.handle(type, p)
    }

    override func send(_ type: Msg, _ payload: Data) {
        guard let s = lock.withLock({ () -> Session? in
            guard let s = session, s.helloSeen else { return nil }
            s.pending += 1
            return s
        }) else { return }
        queue.async {
            guard let keys = s.keys else { return }
            let sealed = WifiCrypto.seal(Link.encode(type, payload), key: keys.send, counter: s.sendCounter)
            s.sendCounter += 1
            var record = Data(capacity: sealed.count + 4)
            record.appendU32(UInt32(sealed.count))
            record.append(sealed)
            s.conn.send(content: record, completion: .contentProcessed { [weak self] _ in
                self?.lock.withLock { s.pending -= 1 }
            })
        }
    }

    private func close(_ s: Session, reason: String) {
        let wasCurrent = lock.withLock { () -> Bool in
            guard session === s else { return false }
            session = nil
            return true
        }
        s.conn.cancel()
        guard wasCurrent, s.helloSeen else { return }
        log("Wi-Fi link closed (\(reason))")
        onDisconnect?()
    }
}
