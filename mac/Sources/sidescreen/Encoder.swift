import CoreMedia
import Foundation
import QuartzCore
import VideoToolbox

// MARK: - Encoder

/// Counters for --stats, shared by the encoder output thread and the logger.
final class Stats {
    private let lock = NSLock()
    private var frames = 0, bytes = 0, encodeMs = 0.0, maxEncodeMs = 0.0, dropped = 0
    private var acks = 0, latencyMs = 0.0, maxLatencyMs = 0.0

    func frame(bytes b: Int, encodeMs ms: Double) {
        lock.withLock { frames += 1; bytes += b; encodeMs += ms; maxEncodeMs = max(maxEncodeMs, ms) }
    }

    func drop() { lock.withLock { dropped += 1 } }

    private var audioPeak: Int16 = 0

    /// Loudest sample sent to the tablet (to tell real sound from silence in --stats).
    func audio(_ pcm: Data) {
        let peak = pcm.withUnsafeBytes { $0.bindMemory(to: Int16.self).map { $0 == .min ? .max : abs($0) }.max() ?? 0 }
        lock.withLock { audioPeak = max(audioPeak, peak) }
    }

    func latency(_ ms: Double) { lock.withLock { acks += 1; latencyMs += ms; maxLatencyMs = max(maxLatencyMs, ms) } }

    func report(seconds: Double) -> String? {
        lock.withLock {
            defer { frames = 0; bytes = 0; encodeMs = 0; maxEncodeMs = 0; dropped = 0; acks = 0; latencyMs = 0; maxLatencyMs = 0; audioPeak = 0 }
            if frames == 0 && audioPeak > 0 { return "audio peak \(audioPeak)" }
            guard frames > 0 else { return nil }
            return String(format: "%.0f fps, encode avg %.1f ms, glass-to-decoded avg %.1f ms / max %.1f ms, %.1f Mbit/s, %d skipped",
                          Double(frames) / seconds, encodeMs / Double(frames),
                          acks > 0 ? latencyMs / Double(acks) : 0, maxLatencyMs,
                          Double(bytes) * 8 / seconds / 1e6, dropped) + (audioPeak > 0 ? ", audio peak \(audioPeak)" : "")
        }
    }
}

enum Codec: UInt32 {
    case h264 = 0, hevc = 1
}

/// Hardware H.264/HEVC encoder tuned for latency: no B-frames, real-time, low-latency rate control.
final class Encoder {
    private var session: VTCompressionSession?
    private let lock = NSLock()
    private var forceKey = true
    let codec: Codec
    var stats: Stats?

    /// (Annex-B access unit, isKeyframe, Annex-B parameter sets on keyframes, time the frame entered the encoder)
    var onFrame: ((Data, Bool, Data?, CFTimeInterval) -> Void)?

    init?(width: Int, height: Int, fps: Int, bitrate: Int, codec: Codec) {
        self.codec = codec
        let spec = [kVTVideoEncoderSpecification_EnableLowLatencyRateControl: true] as CFDictionary
        var s: VTCompressionSession?
        let status = VTCompressionSessionCreate(
            allocator: nil, width: Int32(width), height: Int32(height),
            codecType: codec == .hevc ? kCMVideoCodecType_HEVC : kCMVideoCodecType_H264,
            encoderSpecification: spec,
            imageBufferAttributes: nil, compressedDataAllocator: nil,
            outputCallback: nil, refcon: nil, compressionSessionOut: &s)
        guard status == noErr, let s else {
            log("VTCompressionSessionCreate (\(codec)) failed: \(status)")
            return nil
        }
        func set(_ key: CFString, _ value: CFTypeRef) {
            let st = VTSessionSetProperty(s, key: key, value: value)
            if st != noErr { log("encoder: could not set \(key) (\(st))") }
        }
        set(kVTCompressionPropertyKey_RealTime, kCFBooleanTrue)
        set(kVTCompressionPropertyKey_AllowFrameReordering, kCFBooleanFalse)
        set(kVTCompressionPropertyKey_ProfileLevel,
            codec == .hevc ? kVTProfileLevel_HEVC_Main_AutoLevel : kVTProfileLevel_H264_ConstrainedHigh_AutoLevel)
        set(kVTCompressionPropertyKey_AverageBitRate, NSNumber(value: bitrate))
        set(kVTCompressionPropertyKey_ExpectedFrameRate, NSNumber(value: fps))
        set(kVTCompressionPropertyKey_MaxKeyFrameIntervalDuration, NSNumber(value: 10))
        VTCompressionSessionPrepareToEncodeFrames(s)
        session = s
    }

    deinit {
        if let session { VTCompressionSessionInvalidate(session) }
    }

    func requestKeyframe() { lock.withLock { forceKey = true } }

    /// Change the target bitrate on the fly (adaptive bitrate).
    func setBitrate(_ bps: Int) {
        guard let session else { return }
        VTSessionSetProperty(session, key: kVTCompressionPropertyKey_AverageBitRate, value: NSNumber(value: bps))
    }

    func encode(_ pb: CVPixelBuffer, pts: CMTime) {
        guard let session else { return }
        let key = lock.withLock { () -> Bool in
            defer { forceKey = false }
            return forceKey
        }
        let props = key ? [kVTEncodeFrameOptionKey_ForceKeyFrame: true] as CFDictionary : nil
        let started = CACurrentMediaTime()
        VTCompressionSessionEncodeFrame(
            session, imageBuffer: pb, presentationTimeStamp: pts, duration: .invalid,
            frameProperties: props, infoFlagsOut: nil
        ) { [weak self] status, _, sb in
            guard status == noErr, let sb, let self else { return }
            self.emit(sb, started: started)
        }
    }

    private func emit(_ sb: CMSampleBuffer, started: CFTimeInterval) {
        var isKey = true
        if let atts = CMSampleBufferGetSampleAttachmentsArray(sb, createIfNecessary: false) as? [[CFString: Any]],
           let notSync = atts.first?[kCMSampleAttachmentKey_NotSync] as? Bool {
            isKey = !notSync
        }

        var config: Data?
        if isKey, let fmt = CMSampleBufferGetFormatDescription(sb) {
            // H.264: SPS, PPS. HEVC: VPS, SPS, PPS.
            let getParameterSet = codec == .hevc
                ? CMVideoFormatDescriptionGetHEVCParameterSetAtIndex
                : CMVideoFormatDescriptionGetH264ParameterSetAtIndex
            var count = 0
            _ = getParameterSet(fmt, 0, nil, nil, &count, nil)
            var d = Data()
            for i in 0..<count {
                var ptr: UnsafePointer<UInt8>?
                var size = 0
                if getParameterSet(fmt, i, &ptr, &size, nil, nil) == noErr, let ptr {
                    d.append(contentsOf: startCode)
                    d.append(ptr, count: size)
                }
            }
            config = d
        }

        // AVCC (4-byte big-endian NAL lengths) -> Annex-B (start codes).
        guard let bb = CMSampleBufferGetDataBuffer(sb) else { return }
        let total = CMBlockBufferGetDataLength(bb)
        var avcc = Data(count: total)
        avcc.withUnsafeMutableBytes { _ = CMBlockBufferCopyDataBytes(bb, atOffset: 0, dataLength: total, destination: $0.baseAddress!) }
        var out = Data(capacity: total + 16)
        var i = 0
        while i + 4 <= total {
            let n = Int(avcc.u32(at: i))
            i += 4
            guard i + n <= total else { break }
            out.append(contentsOf: startCode)
            out.append(avcc[i..<(i + n)])
            i += n
        }
        stats?.frame(bytes: out.count, encodeMs: (CACurrentMediaTime() - started) * 1000)
        onFrame?(out, isKey, config, started)
    }
}
