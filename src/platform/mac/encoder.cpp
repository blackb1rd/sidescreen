// Hardware H.264/HEVC encoding on macOS (VideoToolbox), tuned for latency: real-time, no
// B-frames, low-latency rate control. Output is Annex-B for the tablet's MediaCodec.
#include "core/log.hpp"
#include "platform/platform.hpp"

#include <CoreMedia/CoreMedia.h>
#include <VideoToolbox/VideoToolbox.h>

#include <atomic>
#include <mutex>

namespace spanly::platform {

namespace {

constexpr uint8_t kStartCode[] = {0, 0, 0, 1};

/// Scales a frame to the encoder's size on the GPU: needed briefly when the stream changes size
/// (e.g. the tablet moved to Wi-Fi) while frames of the old size are still arriving.
class Scaler {
public:
    ~Scaler() {
        if (session_) VTPixelTransferSessionInvalidate(session_), CFRelease(session_);
        if (pool_) CVPixelBufferPoolRelease(pool_);
    }

    CVPixelBufferRef scale(CVPixelBufferRef in, int w, int h) {
        if (!session_) VTPixelTransferSessionCreate(nullptr, &session_);
        if (!pool_ || w != w_ || h != h_) {
            if (pool_) CVPixelBufferPoolRelease(pool_);
            pool_ = nullptr;
            int format = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
            CFNumberRef f = CFNumberCreate(nullptr, kCFNumberIntType, &format);
            CFNumberRef cw = CFNumberCreate(nullptr, kCFNumberIntType, &w);
            CFNumberRef ch = CFNumberCreate(nullptr, kCFNumberIntType, &h);
            CFDictionaryRef io = CFDictionaryCreate(nullptr, nullptr, nullptr, 0, &kCFTypeDictionaryKeyCallBacks,
                                                    &kCFTypeDictionaryValueCallBacks);
            const void* keys[] = {kCVPixelBufferPixelFormatTypeKey, kCVPixelBufferWidthKey, kCVPixelBufferHeightKey,
                                  kCVPixelBufferIOSurfacePropertiesKey};
            const void* values[] = {f, cw, ch, io};
            CFDictionaryRef attrs = CFDictionaryCreate(nullptr, keys, values, 4, &kCFTypeDictionaryKeyCallBacks,
                                                       &kCFTypeDictionaryValueCallBacks);
            CVPixelBufferPoolCreate(nullptr, nullptr, attrs, &pool_);
            for (CFTypeRef o : {CFTypeRef(f), CFTypeRef(cw), CFTypeRef(ch), CFTypeRef(io), CFTypeRef(attrs)})
                CFRelease(o);
            w_ = w;
            h_ = h;
        }
        CVPixelBufferRef out = nullptr;
        if (!session_ || !pool_ || CVPixelBufferPoolCreatePixelBuffer(nullptr, pool_, &out) != kCVReturnSuccess)
            return nullptr;
        if (VTPixelTransferSessionTransferImage(session_, in, out) != noErr) {
            CVPixelBufferRelease(out);
            return nullptr;
        }
        return out;
    }

private:
    VTPixelTransferSessionRef session_ = nullptr;
    CVPixelBufferPoolRef pool_ = nullptr;
    int w_ = 0, h_ = 0;
};

class MacEncoder : public Encoder {
public:
    MacEncoder(VTCompressionSessionRef s, Codec codec, int w, int h) : session_(s), codec_(codec), w_(w), h_(h) {}
    ~MacEncoder() override {
        VTCompressionSessionInvalidate(session_);
        CFRelease(session_);
    }

    Codec codec() const override { return codec_; }
    int width() const override { return w_; }
    int height() const override { return h_; }
    void requestKeyframe() override { forceKey_ = true; }

    void setBitrate(int bps) override {
        CFNumberRef n = CFNumberCreate(nullptr, kCFNumberIntType, &bps);
        VTSessionSetProperty(session_, kVTCompressionPropertyKey_AverageBitRate, n);
        CFRelease(n);
    }

    void encode(const Frame& frame) override {
        auto pb = static_cast<CVPixelBufferRef>(frame.image.get());
        CVPixelBufferRef scaled = nullptr;
        if (frame.width != w_ || frame.height != h_) {
            std::scoped_lock l(scalerLock_);
            scaled = scaler_.scale(pb, w_, h_);
            if (!scaled) return;
            pb = scaled;
        }
        CFDictionaryRef props = nullptr;
        if (forceKey_.exchange(false)) {
            const void* keys[] = {kVTEncodeFrameOptionKey_ForceKeyFrame};
            const void* values[] = {kCFBooleanTrue};
            props = CFDictionaryCreate(nullptr, keys, values, 1, &kCFTypeDictionaryKeyCallBacks,
                                       &kCFTypeDictionaryValueCallBacks);
        }
        Clock::time_point captured = frame.captured;
        VTCompressionSessionEncodeFrameWithOutputHandler(session_, pb, CMClockGetTime(CMClockGetHostTimeClock()),
                                                         kCMTimeInvalid, props, nullptr,
                                                         ^(OSStatus status, VTEncodeInfoFlags, CMSampleBufferRef sb) {
                                                           if (status == noErr && sb) emit(sb, captured);
                                                         });
        if (props) CFRelease(props);
        if (scaled) CVPixelBufferRelease(scaled);
    }

private:
    void emit(CMSampleBufferRef sb, Clock::time_point captured) {
        bool key = true;
        if (CFArrayRef atts = CMSampleBufferGetSampleAttachmentsArray(sb, false); atts && CFArrayGetCount(atts) > 0) {
            auto a = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(atts, 0));
            auto notSync = static_cast<CFBooleanRef>(CFDictionaryGetValue(a, kCMSampleAttachmentKey_NotSync));
            key = !(notSync && CFBooleanGetValue(notSync));
        }
        std::optional<Bytes> config;
        if (CMFormatDescriptionRef fmt = CMSampleBufferGetFormatDescription(sb); key && fmt) {
            // H.264: SPS, PPS. HEVC: VPS, SPS, PPS.
            auto get = codec_ == Codec::Hevc ? CMVideoFormatDescriptionGetHEVCParameterSetAtIndex
                                             : CMVideoFormatDescriptionGetH264ParameterSetAtIndex;
            size_t count = 0;
            get(fmt, 0, nullptr, nullptr, &count, nullptr);
            config.emplace();
            for (size_t i = 0; i < count; ++i) {
                const uint8_t* p = nullptr;
                size_t n = 0;
                if (get(fmt, i, &p, &n, nullptr, nullptr) != noErr || !p) continue;
                config->insert(config->end(), std::begin(kStartCode), std::end(kStartCode));
                config->insert(config->end(), p, p + n);
            }
        }
        // AVCC (4-byte big-endian NAL lengths) -> Annex-B (start codes).
        CMBlockBufferRef bb = CMSampleBufferGetDataBuffer(sb);
        if (!bb) return;
        size_t total = CMBlockBufferGetDataLength(bb);
        Bytes avcc(total);
        CMBlockBufferCopyDataBytes(bb, 0, total, avcc.data());
        Bytes out;
        out.reserve(total + 16);
        for (size_t i = 0; i + 4 <= total;) {
            size_t n = u32At(avcc, i);
            i += 4;
            if (i + n > total) break;
            out.insert(out.end(), std::begin(kStartCode), std::end(kStartCode));
            out.insert(out.end(), avcc.begin() + ptrdiff_t(i), avcc.begin() + ptrdiff_t(i + n));
            i += n;
        }
        if (onFrame) onFrame(std::move(out), key, std::move(config), captured);
    }

    VTCompressionSessionRef session_;
    Codec codec_;
    int w_, h_;
    std::atomic<bool> forceKey_{true};
    std::mutex scalerLock_;
    Scaler scaler_;
};

void setNumber(VTCompressionSessionRef s, CFStringRef key, int value) {
    CFNumberRef n = CFNumberCreate(nullptr, kCFNumberIntType, &value);
    if (VTSessionSetProperty(s, key, n) != noErr) log("encoder: could not set a property");
    CFRelease(n);
}

} // namespace

std::unique_ptr<Encoder> Encoder::create(int width, int height, int fps, int bitrate, Codec codec) {
    const void* keys[] = {kVTVideoEncoderSpecification_EnableLowLatencyRateControl};
    const void* values[] = {kCFBooleanTrue};
    CFDictionaryRef spec =
        CFDictionaryCreate(nullptr, keys, values, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    VTCompressionSessionRef s = nullptr;
    OSStatus status = VTCompressionSessionCreate(nullptr, width, height,
                                                 codec == Codec::Hevc ? kCMVideoCodecType_HEVC : kCMVideoCodecType_H264,
                                                 spec, nullptr, nullptr, nullptr, nullptr, &s);
    CFRelease(spec);
    if (status != noErr || !s) {
        log("VTCompressionSessionCreate ({}) failed: {}", codec == Codec::Hevc ? "hevc" : "h264", status);
        return nullptr;
    }
    VTSessionSetProperty(s, kVTCompressionPropertyKey_RealTime, kCFBooleanTrue);
    VTSessionSetProperty(s, kVTCompressionPropertyKey_AllowFrameReordering, kCFBooleanFalse);
    VTSessionSetProperty(s, kVTCompressionPropertyKey_ProfileLevel,
                         codec == Codec::Hevc ? kVTProfileLevel_HEVC_Main_AutoLevel
                                              : kVTProfileLevel_H264_ConstrainedHigh_AutoLevel);
    setNumber(s, kVTCompressionPropertyKey_AverageBitRate, bitrate);
    setNumber(s, kVTCompressionPropertyKey_ExpectedFrameRate, fps);
    setNumber(s, kVTCompressionPropertyKey_MaxKeyFrameIntervalDuration, 10);
    VTCompressionSessionPrepareToEncodeFrames(s);
    return std::make_unique<MacEncoder>(s, codec, width, height);
}

} // namespace spanly::platform
