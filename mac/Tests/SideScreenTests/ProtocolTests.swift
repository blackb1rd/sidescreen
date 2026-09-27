import Foundation
import Testing
@testable import sidescreen

/// Records what a Link parses instead of acting on it.
private final class RecordingLink: Link {
    var touches: [(UInt8, Float, Float)] = []
    var acks: [UInt32] = []
    var hellos: [(Int, Int, Int)] = []

    override init() {
        super.init()
        onTouch = { [unowned self] a, x, y in touches.append((a, x, y)) }
        onAck = { [unowned self] id in acks.append(id) }
        onHello = { [unowned self] w, h, dpi, _, _, _ in hellos.append((w, h, dpi)) }
    }
}

private func touch(_ action: UInt8, _ x: Float, _ y: Float) -> Data {
    var p = Data([action])
    p.appendU32(x.bitPattern)
    p.appendU32(y.bitPattern)
    return Link.encode(.touch, p)
}

private func ack(_ id: UInt32) -> Data {
    var p = Data()
    p.appendU32(id)
    return Link.encode(.ack, p)
}

@Suite struct ProtocolTests {
    @Test func encodeWritesMarkerTypeAndLength() {
        let d = Link.encode(.display, Data([1]))
        #expect(Array(d) == [0x5A, Msg.display.rawValue, 0, 0, 0, 1, 1])
    }

    @Test func parsesBackToBackMessages() {
        let link = RecordingLink()
        var buf = touch(0, 0.25, 0.75) + ack(7) + ack(8)
        link.consume(&buf)
        #expect(buf.isEmpty)
        #expect(link.touches.count == 1)
        #expect(link.touches.first?.1 == 0.25)
        #expect(link.acks == [7, 8])
    }

    @Test func waitsForTheRestOfASplitMessage() {
        let link = RecordingLink()
        let whole = ack(42)
        var buf = whole.prefix(4)
        link.consume(&buf)
        #expect(link.acks.isEmpty)
        #expect(buf.count == 4)
        buf.append(whole.suffix(from: 4))
        link.consume(&buf)
        #expect(link.acks == [42])
    }

    @Test func resynchronisesAfterGarbage() {
        let link = RecordingLink()
        // Stale bytes from an earlier session, including a fake marker with an absurd length.
        var buf = Data([0x01, 0x5A, 0x0C, 0xFF, 0xFF, 0xFF, 0xFF, 0x33]) + ack(5)
        link.consume(&buf)
        #expect(link.acks == [5])
        #expect(buf.isEmpty)
    }

    @Test func rejectsImplausibleHeaders() {
        #expect(Link.plausible(Msg.ack.rawValue, 4))
        #expect(!Link.plausible(Msg.ack.rawValue, 5))
        #expect(Link.plausible(Msg.hello.rawValue, 24))
        #expect(!Link.plausible(Msg.hello.rawValue, 4096))
        #expect(!Link.plausible(Msg.frame.rawValue, 100)) // Mac -> tablet only
        #expect(!Link.plausible(0xEE, 0))
    }

    @Test func parsesHello() {
        let link = RecordingLink()
        var p = Data()
        for v: UInt32 in [2560, 1600, 360, 1, 2304, 1440] { p.appendU32(v) }
        var buf = Link.encode(.hello, p)
        link.consume(&buf)
        #expect(link.hellos.count == 1)
        #expect(link.hellos.first?.0 == 2560 && link.hellos.first?.2 == 360)
    }
}
