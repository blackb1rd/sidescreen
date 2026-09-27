import Foundation
import Network

// MARK: - TCP link (adb reverse fallback)

/// Single-client TCP server on loopback. A new connection replaces the old one.
final class Server: Link {
    private let listener: NWListener
    private let queue = DispatchQueue(label: "server")
    private let lock = NSLock()
    private var conn: NWConnection?
    private var pending = 0
    private lazy var heartbeat = DispatchSource.makeTimerSource(queue: queue)

    init(port: UInt16) throws {
        let tcp = NWProtocolTCP.Options()
        tcp.noDelay = true
        let params = NWParameters(tls: nil, tcp: tcp)
        params.requiredInterfaceType = .loopback
        params.allowLocalEndpointReuse = true
        listener = try NWListener(using: params, on: NWEndpoint.Port(rawValue: port)!)
        super.init()
        listener.newConnectionHandler = { [weak self] c in self?.accept(c) }
        listener.stateUpdateHandler = { state in
            if case let .failed(e) = state {
                log("listener failed: \(e)")
                exit(1)
            }
        }
        listener.start(queue: queue)
        // An idle screen sends nothing; the tablet treats 4 s of silence as a dead link.
        heartbeat.schedule(deadline: .now() + 0.5, repeating: 0.5)
        heartbeat.setEventHandler { [weak self] in
            guard let self, self.isConnected else { return }
            self.send(.nop, Data())
        }
        heartbeat.resume()
    }

    override var isConnected: Bool { lock.withLock { conn != nil } }

    override var backlog: Int { lock.withLock { pending } }

    private func accept(_ c: NWConnection) {
        lock.withLock {
            conn?.cancel()
            conn = c
            pending = 0
        }
        c.stateUpdateHandler = { [weak self, weak c] state in
            guard let self, let c else { return }
            switch state {
            case .ready:
                log("tablet connected")
                self.onClient?()
            case .failed, .cancelled:
                let wasCurrent = self.lock.withLock { () -> Bool in
                    guard self.conn === c else { return false }
                    self.conn = nil
                    return true
                }
                if wasCurrent {
                    log("tablet disconnected")
                    self.onDisconnect?()
                }
            default:
                break
            }
        }
        c.start(queue: queue)
        receive(c, buffer: Data())
    }

    private func receive(_ c: NWConnection, buffer: Data) {
        c.receive(minimumIncompleteLength: 1, maximumLength: 1 << 16) { [weak self] data, _, done, error in
            guard let self else { return }
            var buf = buffer
            if let data { buf.append(data) }
            self.consume(&buf)
            if done || error != nil {
                c.cancel()
                return
            }
            self.receive(c, buffer: buf)
        }
    }

    override func send(_ type: Msg, _ payload: Data) {
        guard let c = lock.withLock({ () -> NWConnection? in
            guard let c = conn else { return nil }
            pending += 1
            return c
        }) else { return }
        c.send(content: Link.encode(type, payload), completion: .contentProcessed { [weak self, weak c] _ in
            guard let self else { return }
            self.lock.withLock { if self.conn === c { self.pending -= 1 } }
        })
    }
}
