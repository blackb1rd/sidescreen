import Foundation

// MARK: - Protocol

enum Msg: UInt8 {
    case nop = 0 // padding / heartbeat, ignored by the receiver
    case size = 1, config = 2, frame = 3, display = 4
    case helloRequest = 5 // "please (re)send HELLO" — lets a link recover a session the tablet still holds
    case audio = 6 // 48 kHz 16-bit interleaved stereo PCM
    case pair = 7 // Wi-Fi pairing secret (32 bytes) + this Mac's name, sent only over USB
    case shareStart = 8, shareStop = 9 // show / stop showing the tablet's screen on the Mac
    case remotePointer = 20, remoteScroll = 21, remoteKey = 22 // Mac input on the tablet's screen
    case touch = 10, hello = 11, ack = 12, scroll = 13, zoom = 14, pen = 15
    case shareSize = 16, shareConfig = 17, shareFrame = 18 // the tablet's screen, H.264
    case viewing = 19 // the tablet's app shows the Mac's screen (1) or is in the background (0)
    case shareStatus = 23 // state (0 stopped, 1 sharing, 2 declined), control available
}

extension Data {
    mutating func appendU32(_ v: UInt32) {
        append(contentsOf: [UInt8(v >> 24), UInt8(v >> 16 & 0xff), UInt8(v >> 8 & 0xff), UInt8(v & 0xff)])
    }

    func u32(at i: Int) -> UInt32 {
        let b = startIndex + i
        return UInt32(self[b]) << 24 | UInt32(self[b + 1]) << 16 | UInt32(self[b + 2]) << 8 | UInt32(self[b + 3])
    }
}

let startCode: [UInt8] = [0, 0, 0, 1]

// MARK: - Links

/// A connection to the tablet. Subclasses: TCP through `adb reverse` ([Server]) and
/// raw USB accessory mode ([UsbLink]). Both carry the same messages.
class Link {
    var onClient: (() -> Void)?
    var onTouch: ((UInt8, Float, Float) -> Void)?
    var onHello: ((Int, Int, Int, UInt32, Int, Int) -> Void)?
    var onDisconnect: (() -> Void)?
    var onAck: ((UInt32) -> Void)?
    /// (kind, phase, last, x, y, dx, dy) — see Pointer.scroll
    var onScroll: ((UInt8, UInt8, Bool, Float, Float, Float, Float) -> Void)?
    var onZoom: ((Int8, Float, Float) -> Void)?
    /// (action, buttons, x, y, pressure) — see Pointer.pen
    var onPen: ((UInt8, UInt8, Float, Float, Float) -> Void)?
    var onViewing: ((Bool) -> Void)?
    /// The tablet's own screen: (width, height), parameter sets, frames, and (state, control).
    var onShareSize: ((Int, Int) -> Void)?
    var onShareConfig: ((Data) -> Void)?
    var onShareFrame: ((Data) -> Void)?
    var onShareStatus: ((UInt8, Bool) -> Void)?

    var isConnected: Bool { false }

    /// Messages handed off but not yet written out.
    var backlog: Int { 0 }

    func send(_ type: Msg, _ payload: Data) {}

    static let marker: UInt8 = 0x5A
    static let maxRecord = 20 << 20 // largest encrypted Wi-Fi record
    static let headerSize = 6

    /// Header sanity per message type (tablet -> Mac), so stale bytes rarely pass for a message.
    static func plausible(_ type: UInt8, _ len: Int) -> Bool {
        switch Msg(rawValue: type) {
        case .nop: return len == 0
        case .touch, .zoom: return len == 9
        case .hello: return (12...64).contains(len)
        case .ack: return len == 4
        case .scroll: return len == 19
        case .pen: return len == 14
        case .viewing: return len == 1
        case .shareSize: return len == 12
        case .shareConfig: return (1...4096).contains(len)
        case .shareFrame: return (2...(16 << 20)).contains(len)
        case .shareStatus: return len == 2
        default: return false
        }
    }

    static func encode(_ type: Msg, _ payload: Data) -> Data {
        var d = Data(capacity: payload.count + headerSize)
        d.append(marker)
        d.append(type.rawValue)
        d.appendU32(UInt32(payload.count))
        d.append(payload)
        return d
    }

    /// Handle every complete message at the front of `buf`, leaving any partial one.
    /// Bytes that don't start a plausible message are skipped until the stream is back in sync.
    func consume(_ buf: inout Data) {
        var skipped = 0
        while buf.count >= Link.headerSize {
            let b = buf.startIndex
            let len = Int(buf.u32(at: 2))
            guard buf[b] == Link.marker, Link.plausible(buf[b + 1], len) else {
                buf = Data(buf[(b + 1)...])
                skipped += 1
                continue
            }
            guard buf.count >= Link.headerSize + len else { break }
            let type = buf[b + 1]
            let payload = Data(buf[(b + Link.headerSize)..<(b + Link.headerSize + len)])
            buf = Data(buf[(b + Link.headerSize + len)...])
            handle(type, payload)
        }
        if skipped > 0 { log("resynchronised the stream (skipped \(skipped) bytes)") }
    }

    func handle(_ type: UInt8, _ p: Data) {
        if type == Msg.touch.rawValue, p.count >= 9 {
            onTouch?(p[p.startIndex], Float(bitPattern: p.u32(at: 1)), Float(bitPattern: p.u32(at: 5)))
        } else if type == Msg.scroll.rawValue, p.count >= 19 {
            let b = p.startIndex
            onScroll?(p[b], p[b + 1], p[b + 2] != 0,
                      Float(bitPattern: p.u32(at: 3)), Float(bitPattern: p.u32(at: 7)),
                      Float(bitPattern: p.u32(at: 11)), Float(bitPattern: p.u32(at: 15)))
        } else if type == Msg.zoom.rawValue, p.count >= 9 {
            onZoom?(Int8(bitPattern: p[p.startIndex]), Float(bitPattern: p.u32(at: 1)), Float(bitPattern: p.u32(at: 5)))
        } else if type == Msg.pen.rawValue, p.count >= 14 {
            let b = p.startIndex
            onPen?(p[b], p[b + 1], Float(bitPattern: p.u32(at: 2)), Float(bitPattern: p.u32(at: 6)),
                   Float(bitPattern: p.u32(at: 10)))
        } else if type == Msg.viewing.rawValue, p.count >= 1 {
            onViewing?(p[p.startIndex] != 0)
        } else if type == Msg.shareSize.rawValue, p.count >= 8 {
            onShareSize?(Int(p.u32(at: 0)), Int(p.u32(at: 4)))
        } else if type == Msg.shareConfig.rawValue {
            onShareConfig?(p)
        } else if type == Msg.shareFrame.rawValue, p.count >= 2 {
            onShareFrame?(Data(p.dropFirst())) // flags byte, then the access unit
        } else if type == Msg.shareStatus.rawValue, p.count >= 2 {
            onShareStatus?(p[p.startIndex], p[p.startIndex + 1] != 0)
        } else if type == Msg.ack.rawValue, p.count >= 4 {
            onAck?(p.u32(at: 0))
        } else if type == Msg.hello.rawValue, p.count >= 12 {
            onHello?(Int(p.u32(at: 0)), Int(p.u32(at: 4)), Int(p.u32(at: 8)), p.count >= 16 ? p.u32(at: 12) : 0,
                     p.count >= 24 ? Int(p.u32(at: 16)) : 0, p.count >= 24 ? Int(p.u32(at: 20)) : 0)
        }
    }
}
