// A window on the Mac showing the tablet's own screen (the reverse direction), and turning
// mouse and keyboard input into taps, swipes and typing on the tablet.

import AppKit
import AVFoundation
import CoreMedia

/// The video view: decodes and shows the tablet's H.264 stream, and forwards input.
final class TabletView: NSView {
    let displayLayer = AVSampleBufferDisplayLayer()
    var videoSize = CGSize(width: 16, height: 10)
    var send: ((Msg, Data) -> Void)?

    override init(frame: NSRect) {
        super.init(frame: frame)
        wantsLayer = true
        layer = CALayer()
        layer?.backgroundColor = NSColor.black.cgColor
        displayLayer.videoGravity = .resizeAspect
        layer?.addSublayer(displayLayer)
    }

    required init?(coder: NSCoder) { fatalError("not used") }

    override func layout() {
        super.layout()
        displayLayer.frame = bounds
    }

    override var acceptsFirstResponder: Bool { true }

    // MARK: Pointer (Protocol: remotePointer action 0 down, 1 move, 2 up, 3 long press)

    /// Position within the letterboxed video as fractions of the tablet's screen.
    private func position(_ e: NSEvent) -> (Float, Float) {
        let p = convert(e.locationInWindow, from: nil)
        let r = AVMakeRect(aspectRatio: videoSize, insideRect: bounds)
        guard r.width > 0, r.height > 0 else { return (0, 0) }
        let x = Float((p.x - r.minX) / r.width)
        let y = Float(1 - (p.y - r.minY) / r.height) // AppKit's y axis points up
        return (min(max(x, 0), 1), min(max(y, 0), 1))
    }

    private func pointer(_ action: UInt8, _ e: NSEvent) {
        let (x, y) = position(e)
        var p = Data([action])
        p.appendU32(x.bitPattern)
        p.appendU32(y.bitPattern)
        send?(.remotePointer, p)
    }

    override func mouseDown(with e: NSEvent) { pointer(0, e) }
    override func mouseDragged(with e: NSEvent) { pointer(1, e) }
    override func mouseUp(with e: NSEvent) { pointer(2, e) }
    override func rightMouseDown(with e: NSEvent) { pointer(3, e) } // long press

    override func scrollWheel(with e: NSEvent) {
        let r = AVMakeRect(aspectRatio: videoSize, insideRect: bounds)
        guard r.width > 0 else { return }
        let lineScale: CGFloat = e.hasPreciseScrollingDeltas ? 1 : 12 // mouse wheels scroll in lines
        let (x, y) = position(e)
        var p = Data()
        for v in [x, y, Float(e.scrollingDeltaX * lineScale / r.width), Float(e.scrollingDeltaY * lineScale / r.height)] {
            p.appendU32(v.bitPattern)
        }
        send?(.remoteScroll, p)
    }

    // MARK: Keyboard (Protocol: remoteKey kind 0 text, 1 key code, 2 global action)

    /// Mac key codes that map to Android key codes.
    private static let keys: [UInt16: UInt16] = [
        36: 66, 76: 66, // return, enter -> KEYCODE_ENTER
        51: 67, // delete -> KEYCODE_DEL
        117: 112, // forward delete -> KEYCODE_FORWARD_DEL
        48: 61, // tab
        123: 21, 124: 22, 125: 20, 126: 19, // arrows
    ]

    override func keyDown(with e: NSEvent) {
        if e.modifierFlags.contains(.command) { return super.keyDown(with: e) } // Mac shortcuts stay on the Mac
        if e.keyCode == 53 { return globalAction(1) } // esc -> Back
        if let code = TabletView.keys[e.keyCode] {
            send?(.remoteKey, Data([1, UInt8(code >> 8), UInt8(code & 0xff)]))
        } else if let text = e.characters, !text.isEmpty, text.unicodeScalars.allSatisfy({ $0.value >= 32 }) {
            send?(.remoteKey, Data([0]) + Data(text.utf8.prefix(255)))
        }
    }

    /// 1 back, 2 home, 3 recents
    func globalAction(_ action: UInt8) { send?(.remoteKey, Data([2, action])) }
}

final class TabletWindow: NSObject, NSWindowDelegate {
    let window: NSWindow
    let view = TabletView(frame: NSRect(x: 0, y: 0, width: 900, height: 563))
    var onClose: (() -> Void)?
    private let hint = NSTextField(labelWithString: "")
    private let queue = DispatchQueue(label: "tablet-video", qos: .userInteractive)
    private var format: CMVideoFormatDescription? // touched only on `queue`

    init(title: String) {
        window = NSWindow(contentRect: view.frame, styleMask: [.titled, .closable, .miniaturizable, .resizable],
                          backing: .buffered, defer: false)
        super.init()
        window.title = title
        window.contentView = view
        window.isReleasedWhenClosed = false
        window.delegate = self
        window.addTitlebarAccessoryViewController(navigationButtons())
        hint.textColor = .white
        hint.backgroundColor = NSColor.black.withAlphaComponent(0.6)
        hint.drawsBackground = true
        hint.alignment = .center
        hint.isHidden = true
        hint.stringValue = "To control the tablet from here, turn on “SideScreen: control from Mac” in the tablet's Settings › Accessibility."
        hint.translatesAutoresizingMaskIntoConstraints = false
        view.addSubview(hint)
        NSLayoutConstraint.activate([
            hint.leadingAnchor.constraint(equalTo: view.leadingAnchor),
            hint.trailingAnchor.constraint(equalTo: view.trailingAnchor),
            hint.bottomAnchor.constraint(equalTo: view.bottomAnchor),
        ])
        window.center()
    }

    private func navigationButtons() -> NSTitlebarAccessoryViewController {
        func button(_ symbol: String, _ label: String, _ action: UInt8) -> NSButton {
            let b = NSButton(image: NSImage(systemSymbolName: symbol, accessibilityDescription: label)!,
                             target: self, action: #selector(navigate(_:)))
            b.tag = Int(action)
            b.bezelStyle = .texturedRounded
            b.toolTip = label
            return b
        }
        let stack = NSStackView(views: [
            button("chevron.backward", "Back", 1), button("circle", "Home", 2), button("square.on.square", "Recents", 3),
        ])
        stack.edgeInsets = NSEdgeInsets(top: 0, left: 0, bottom: 0, right: 8)
        stack.frame.size = stack.fittingSize // the accessory keeps the view's frame; zero would hide it
        let vc = NSTitlebarAccessoryViewController()
        vc.view = stack
        vc.layoutAttribute = .trailing
        return vc
    }

    @objc private func navigate(_ sender: NSButton) { view.globalAction(UInt8(sender.tag)) }

    func show() {
        NSApp.activate(ignoringOtherApps: true)
        window.makeKeyAndOrderFront(nil)
        window.makeFirstResponder(view)
    }

    func setControlAvailable(_ available: Bool) { hint.isHidden = available }

    /// New stream geometry: shape the window like the tablet's screen.
    func setSize(width: Int, height: Int) {
        let size = CGSize(width: width, height: height)
        view.videoSize = size
        window.contentAspectRatio = size
        let maxH = (window.screen ?? NSScreen.main).map { $0.visibleFrame.height * 0.8 } ?? 700
        let h = min(maxH, CGFloat(height))
        window.setContentSize(NSSize(width: h * size.width / size.height, height: h))
        queue.async { self.format = nil } // wait for the new parameter sets
    }

    // MARK: Decoding

    private static func nalUnits(_ annexB: Data) -> [Data] {
        let b = [UInt8](annexB)
        var starts: [(Int, Int)] = [] // (start code offset, payload offset)
        var i = 0
        while i + 3 <= b.count {
            if b[i] == 0, b[i + 1] == 0, b[i + 2] == 1 {
                starts.append((i > 0 && b[i - 1] == 0 ? i - 1 : i, i + 3))
                i += 3
            } else {
                i += 1
            }
        }
        return starts.enumerated().map { n, s in
            let end = n + 1 < starts.count ? starts[n + 1].0 : b.count
            return Data(b[s.1..<end])
        }
    }

    /// SPS + PPS in Annex-B form (MSG_SHARE_CONFIG).
    func config(_ annexB: Data) {
        queue.async { self.updateFormat(TabletWindow.nalUnits(annexB)) }
    }

    private func updateFormat(_ nals: [Data]) {
        guard let sps = nals.first(where: { $0.first.map { $0 & 0x1f } == 7 }),
              let pps = nals.first(where: { $0.first.map { $0 & 0x1f } == 8 }) else { return }
        var fmt: CMVideoFormatDescription?
        sps.withUnsafeBytes { s in
            pps.withUnsafeBytes { p in
                let pointers = [s.bindMemory(to: UInt8.self).baseAddress!, p.bindMemory(to: UInt8.self).baseAddress!]
                let sizes = [sps.count, pps.count]
                CMVideoFormatDescriptionCreateFromH264ParameterSets(
                    allocator: nil, parameterSetCount: 2, parameterSetPointers: pointers,
                    parameterSetSizes: sizes, nalUnitHeaderLength: 4, formatDescriptionOut: &fmt)
            }
        }
        format = fmt
    }

    /// One access unit in Annex-B form (MSG_SHARE_FRAME).
    func frame(_ annexB: Data) {
        queue.async {
            let nals = TabletWindow.nalUnits(annexB)
            if nals.contains(where: { $0.first.map { $0 & 0x1f } == 7 }) { self.updateFormat(nals) }
            guard let format = self.format else { return }
            // AVCC: each NAL unit prefixed with its 4-byte length; parameter sets and AUDs dropped.
            var avcc = Data()
            for nal in nals where ![7, 8, 9].contains(nal.first.map { $0 & 0x1f } ?? 0) {
                avcc.appendU32(UInt32(nal.count))
                avcc.append(nal)
            }
            guard !avcc.isEmpty, let sb = TabletWindow.sampleBuffer(avcc, format) else { return }
            let renderer = self.view.displayLayer.sampleBufferRenderer
            if renderer.status == .failed { renderer.flush() }
            renderer.enqueue(sb)
        }
    }

    private static func sampleBuffer(_ avcc: Data, _ format: CMVideoFormatDescription) -> CMSampleBuffer? {
        var block: CMBlockBuffer?
        guard CMBlockBufferCreateWithMemoryBlock(
            allocator: nil, memoryBlock: nil, blockLength: avcc.count, blockAllocator: nil, customBlockSource: nil,
            offsetToData: 0, dataLength: avcc.count, flags: kCMBlockBufferAssureMemoryNowFlag, blockBufferOut: &block) == noErr,
            let block else { return nil }
        let copied = avcc.withUnsafeBytes {
            CMBlockBufferReplaceDataBytes(with: $0.baseAddress!, blockBuffer: block, offsetIntoDestination: 0, dataLength: avcc.count)
        }
        guard copied == noErr else { return nil }
        var sb: CMSampleBuffer?
        var size = avcc.count
        guard CMSampleBufferCreateReady(
            allocator: nil, dataBuffer: block, formatDescription: format, sampleCount: 1, sampleTimingEntryCount: 0,
            sampleTimingArray: nil, sampleSizeEntryCount: 1, sampleSizeArray: &size, sampleBufferOut: &sb) == noErr,
            let sb else { return nil }
        // Show each frame as soon as it's decoded; there is no playback clock here.
        if let atts = CMSampleBufferGetSampleAttachmentsArray(sb, createIfNecessary: true), CFArrayGetCount(atts) > 0 {
            let dict = unsafeBitCast(CFArrayGetValueAtIndex(atts, 0), to: CFMutableDictionary.self)
            CFDictionarySetValue(dict, Unmanaged.passUnretained(kCMSampleAttachmentKey_DisplayImmediately).toOpaque(),
                                 Unmanaged.passUnretained(kCFBooleanTrue).toOpaque())
        }
        return sb
    }

    func windowWillClose(_ notification: Notification) { onClose?() }
}
