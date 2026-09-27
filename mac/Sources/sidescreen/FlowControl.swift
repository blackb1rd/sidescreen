import Foundation
import QuartzCore

/// Frames sent to the tablet but not yet decoded there (it ACKs each one), and the
/// capture-to-decoded latency those ACKs reveal. Thread-safe: frames are registered on the
/// encoder's thread and ACKs arrive on the link's thread.
final class FlowControl {
    /// 3 suits USB; Wi-Fi's longer round trip needs more frames in flight to reach 60 fps.
    var maxInFlight: UInt32 = 3
    private let lock = NSLock()
    private var nextId: UInt32 = 1
    private var lastAcked: UInt32 = 0
    private var lastAckTime = CACurrentMediaTime()
    private var sentAt: [UInt32: CFTimeInterval] = [:]
    // Totals since the last takeWindow(), for adaptive bitrate.
    private var windowAcks = 0
    private var windowLatencyMs = 0.0
    private var windowSkipped = 0

    /// Forget everything in flight (new session or restart).
    func reset() {
        lock.withLock {
            lastAcked = nextId &- 1
            lastAckTime = CACurrentMediaTime()
            sentAt.removeAll()
        }
    }

    /// Frames acknowledged, their average latency, and captures skipped since the last call.
    func takeWindow() -> (acks: Int, avgLatencyMs: Double, skipped: Int) {
        lock.withLock {
            defer { windowAcks = 0; windowLatencyMs = 0; windowSkipped = 0 }
            return (windowAcks, windowAcks > 0 ? windowLatencyMs / Double(windowAcks) : 0, windowSkipped)
        }
    }

    /// Id for a frame about to be sent; `started` is when it entered the encoder.
    func register(started: CFTimeInterval) -> UInt32 {
        lock.withLock {
            let id = nextId
            nextId &+= 1
            sentAt[id] = started
            return id
        }
    }

    /// The tablet decoded frame `id` (and so everything before it). Returns its latency in ms.
    func acked(_ id: UInt32) -> Double? {
        let started: CFTimeInterval? = lock.withLock {
            guard id > lastAcked else { return nil }
            lastAcked = id
            lastAckTime = CACurrentMediaTime()
            let t = sentAt[id]
            sentAt = sentAt.filter { $0.key > id }
            return t
        }
        guard let started else { return nil }
        let ms = (CACurrentMediaTime() - started) * 1000
        lock.withLock {
            windowAcks += 1
            windowLatencyMs += ms
        }
        return ms
    }

    /// Skip capturing while the tablet is behind. If ACKs stop (a dropped frame, a reconnect),
    /// stop waiting after 250 ms so the stream can't stall.
    func tooManyInFlight() -> Bool {
        lock.withLock {
            if nextId &- 1 &- lastAcked < maxInFlight { return false }
            if CACurrentMediaTime() - lastAckTime > 0.25 {
                lastAcked = nextId &- 1
                lastAckTime = CACurrentMediaTime()
                sentAt.removeAll()
                return false
            }
            windowSkipped += 1
            return true
        }
    }
}
