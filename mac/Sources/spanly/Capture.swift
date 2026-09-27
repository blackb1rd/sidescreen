import CoreMedia
import Foundation
import ScreenCaptureKit

// MARK: - Capture

final class Capture: NSObject, SCStreamOutput, SCStreamDelegate {
    private var stream: SCStream?
    private var config: SCStreamConfiguration?
    private var showingCursor = false
    private let queue = DispatchQueue(label: "capture", qos: .userInteractive)
    private var lastBuffer: CVPixelBuffer?

    var onFrame: ((CVPixelBuffer, CMTime) -> Void)?
    var onStop: (() -> Void)?

    var onAudio: ((Data) -> Void)?
    private let audioQueue = DispatchQueue(label: "capture-audio", qos: .userInteractive)

    func start(displayID: CGDirectDisplayID, width: Int, height: Int, fps: Int, audio: Bool) async throws {
        let content = try await SCShareableContent.excludingDesktopWindows(false, onScreenWindowsOnly: true)
        guard let display = content.displays.first(where: { $0.displayID == displayID }) else {
            throw NSError(domain: "spanly", code: 1, userInfo: [NSLocalizedDescriptionKey: "display not shareable"])
        }
        let cfg = SCStreamConfiguration()
        cfg.width = width
        cfg.height = height
        cfg.minimumFrameInterval = CMTime(value: 1, timescale: CMTimeScale(fps))
        cfg.pixelFormat = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange
        cfg.queueDepth = 5
        cfg.showsCursor = false // turned on by setShowsCursor when the real pointer is on this display
        if audio {
            cfg.capturesAudio = true
            cfg.sampleRate = 48_000
            cfg.channelCount = 2
            cfg.excludesCurrentProcessAudio = true
        }
        let s = SCStream(filter: SCContentFilter(display: display, excludingWindows: []), configuration: cfg, delegate: self)
        try s.addStreamOutput(self, type: .screen, sampleHandlerQueue: queue)
        if audio { try s.addStreamOutput(self, type: .audio, sampleHandlerQueue: audioQueue) }
        try await s.startCapture()
        stream = s
        config = cfg
        showingCursor = false
    }

    /// Include the cursor in the picture only when it's useful (see Controller.updateCursorVisibility).
    func setShowsCursor(_ show: Bool) {
        guard show != showingCursor, let stream, let config else { return }
        showingCursor = show
        config.showsCursor = show
        Task { try? await stream.updateConfiguration(config) }
    }

    /// Capture at another size without stopping (the stream moved to a different link).
    func resize(width: Int, height: Int) async {
        guard let stream, let config else { return }
        config.width = width
        config.height = height
        try? await stream.updateConfiguration(config)
    }

    func stop() async {
        try? await stream?.stopCapture()
        stream = nil
        queue.sync { lastBuffer = nil }
    }

    /// ScreenCaptureKit only delivers frames when the screen changes; re-encode the
    /// last one so a newly connected tablet gets a picture immediately.
    func resendLast() {
        queue.async {
            if let pb = self.lastBuffer { self.onFrame?(pb, CMClockGetTime(CMClockGetHostTimeClock())) }
        }
    }

    func stream(_ stream: SCStream, didOutputSampleBuffer sb: CMSampleBuffer, of type: SCStreamOutputType) {
        if type == .audio {
            if sb.isValid, let pcm = AudioPCM.interleavedInt16(sb) { onAudio?(pcm) }
            return
        }
        guard type == .screen, sb.isValid,
              let atts = CMSampleBufferGetSampleAttachmentsArray(sb, createIfNecessary: false) as? [[SCStreamFrameInfo: Any]],
              let raw = atts.first?[.status] as? Int, SCFrameStatus(rawValue: raw) == .complete,
              let pb = CMSampleBufferGetImageBuffer(sb) else { return }
        lastBuffer = pb
        onFrame?(pb, CMSampleBufferGetPresentationTimeStamp(sb))
    }

    func stream(_ stream: SCStream, didStopWithError error: Error) {
        log("capture stopped: \(error.localizedDescription)")
        onStop?()
    }
}
