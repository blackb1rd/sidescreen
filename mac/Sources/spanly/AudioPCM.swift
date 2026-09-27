import CoreMedia
import Foundation

/// Converts ScreenCaptureKit audio (32-bit float, usually one buffer per channel) into the
/// 48 kHz 16-bit interleaved stereo PCM the tablet plays. Raw PCM is ~1.5 Mbit/s: trivial for
/// USB and Wi-Fi, and it adds no encoding delay.
enum AudioPCM {
    static func interleavedInt16(_ sb: CMSampleBuffer) -> Data? {
        guard let fmt = CMSampleBufferGetFormatDescription(sb),
              let asbd = CMAudioFormatDescriptionGetStreamBasicDescription(fmt)?.pointee,
              asbd.mFormatFlags & kAudioFormatFlagIsFloat != 0, asbd.mBitsPerChannel == 32 else { return nil }

        var size = 0
        CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer(
            sb, bufferListSizeNeededOut: &size, bufferListOut: nil, bufferListSize: 0,
            blockBufferAllocator: nil, blockBufferMemoryAllocator: nil, flags: 0, blockBufferOut: nil)
        let raw = UnsafeMutableRawPointer.allocate(byteCount: size, alignment: 16)
        defer { raw.deallocate() }
        let list = raw.bindMemory(to: AudioBufferList.self, capacity: 1)
        var block: CMBlockBuffer?
        guard CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer(
            sb, bufferListSizeNeededOut: nil, bufferListOut: list, bufferListSize: size,
            blockBufferAllocator: nil, blockBufferMemoryAllocator: nil,
            flags: kCMSampleBufferFlag_AudioBufferList_Assure16ByteAlignment, blockBufferOut: &block) == noErr
        else { return nil }

        let buffers = UnsafeMutableAudioBufferListPointer(list)
        let frames = CMSampleBufferGetNumSamples(sb)
        let channels = Int(asbd.mChannelsPerFrame)
        let planar = asbd.mFormatFlags & kAudioFormatFlagIsNonInterleaved != 0
        guard frames > 0, channels > 0, buffers.count > 0 else { return nil }

        var out = Data(count: frames * 4)
        out.withUnsafeMutableBytes { dst in
            let d = dst.bindMemory(to: Int16.self)
            for c in 0..<2 {
                let src = min(c, channels - 1) // mono -> both speakers
                let samples = (planar ? buffers[src] : buffers[0]).mData?.assumingMemoryBound(to: Float.self)
                guard let samples else { continue }
                for f in 0..<frames {
                    let v = planar ? samples[f] : samples[f * channels + src]
                    d[f * 2 + c] = Int16(max(-1, min(1, v)) * 32767).littleEndian
                }
            }
        }
        return out
    }
}
