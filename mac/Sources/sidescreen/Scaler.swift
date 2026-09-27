import CoreVideo
import VideoToolbox

/// Scales a captured frame to the encoder's size on the GPU. Needed only briefly: when the
/// stream changes size (e.g. the tablet moved to Wi-Fi), frames captured at the old size are
/// still arriving. Capture queue only.
final class Scaler {
    private var session: VTPixelTransferSession?
    private var pool: CVPixelBufferPool?
    private var poolSize = (w: 0, h: 0)

    func scale(_ pb: CVPixelBuffer, width w: Int, height h: Int) -> CVPixelBuffer? {
        if session == nil { VTPixelTransferSessionCreate(allocator: nil, pixelTransferSessionOut: &session) }
        if pool == nil || poolSize != (w, h) {
            let attrs: [CFString: Any] = [
                kCVPixelBufferPixelFormatTypeKey: kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange,
                kCVPixelBufferWidthKey: w,
                kCVPixelBufferHeightKey: h,
                kCVPixelBufferIOSurfacePropertiesKey: [:] as CFDictionary,
            ]
            pool = nil
            CVPixelBufferPoolCreate(nil, nil, attrs as CFDictionary, &pool)
            poolSize = (w, h)
        }
        var out: CVPixelBuffer?
        guard let session, let pool, CVPixelBufferPoolCreatePixelBuffer(nil, pool, &out) == kCVReturnSuccess,
              let out, VTPixelTransferSessionTransferImage(session, from: pb, to: out) == noErr else { return nil }
        return out
    }
}
