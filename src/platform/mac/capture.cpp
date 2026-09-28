// Screen and sound capture on macOS: ScreenCaptureKit, driven through the Objective-C runtime.
// Frames arrive as CVPixelBuffers (NV12, GPU memory) on a serial queue; audio as float PCM,
// converted to the 16-bit stereo the tablet plays.
#include "core/log.hpp"
#include "platform/mac/objc.hpp"
#include "platform/platform.hpp"

#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <dispatch/dispatch.h>

#include <algorithm>
#include <atomic>
#include <mutex>

extern "C" id SCStreamFrameInfoStatus; // NSString key in ScreenCaptureKit

namespace spanly::platform {

using namespace mac;

namespace {

/// ScreenCaptureKit audio (32-bit float, usually one buffer per channel) as 48 kHz 16-bit
/// interleaved stereo. Raw PCM is ~1.5 Mbit/s and adds no encoding delay.
std::optional<Bytes> interleavedInt16(CMSampleBufferRef sb) {
    CMFormatDescriptionRef fmt = CMSampleBufferGetFormatDescription(sb);
    const AudioStreamBasicDescription* asbd = fmt ? CMAudioFormatDescriptionGetStreamBasicDescription(fmt) : nullptr;
    if (!asbd || !(asbd->mFormatFlags & kAudioFormatFlagIsFloat) || asbd->mBitsPerChannel != 32) return std::nullopt;
    size_t size = 0;
    CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer(sb, &size, nullptr, 0, nullptr, nullptr, 0, nullptr);
    std::vector<uint8_t> raw(size + 16);
    auto* list = reinterpret_cast<AudioBufferList*>(raw.data());
    CMBlockBufferRef block = nullptr;
    if (CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer(
            sb, nullptr, list, size, nullptr, nullptr, kCMSampleBufferFlag_AudioBufferList_Assure16ByteAlignment,
            &block) != noErr)
        return std::nullopt;
    auto frames = size_t(CMSampleBufferGetNumSamples(sb));
    auto channels = size_t(asbd->mChannelsPerFrame);
    bool planar = asbd->mFormatFlags & kAudioFormatFlagIsNonInterleaved;
    Bytes out(frames * 4);
    if (frames > 0 && channels > 0 && list->mNumberBuffers > 0) {
        auto* d = reinterpret_cast<int16_t*>(out.data());
        for (size_t c = 0; c < 2; ++c) {
            size_t src = std::min(c, channels - 1); // mono -> both speakers
            const auto* samples = static_cast<const float*>(list->mBuffers[planar ? src : 0].mData);
            if (!samples) continue;
            for (size_t f = 0; f < frames; ++f) {
                float v = planar ? samples[f] : samples[f * channels + src];
                d[f * 2 + c] = int16_t(std::clamp(v, -1.0F, 1.0F) * 32767);
            }
        }
    }
    if (block) CFRelease(block);
    return out;
}

class MacCapture;

/// What the delegate points to: outlives the capture (a stream winding down may still call back),
/// and forgets it when it is destroyed. A few bytes per capture, never freed.
struct Anchor {
    std::mutex m;
    MacCapture* capture = nullptr;
};

class MacCapture : public Capture {
public:
    MacCapture() {
        auto attr = dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INTERACTIVE, 0);
        video_ = dispatch_queue_create("spanly.capture", attr);
        audio_ = dispatch_queue_create("spanly.capture-audio", attr);
        anchor_ = new Anchor{};
        anchor_->capture = this;
        delegate_ = Ref(instance(delegateClass(), anchor_));
    }

    ~MacCapture() override {
        std::scoped_lock l(anchor_->m);
        anchor_->capture = nullptr;
    }

    void start(DisplayId display, int width, int height, int fps, bool audio,
               std::function<void(const std::string&)> done) override {
        Pool pool;
        void (^handler)(Obj, Obj) = ^(Obj content, Obj error) {
          if (!content) {
              done(error ? toString(send(error, "localizedDescription")) : "no shareable content");
              return;
          }
          begin(content, display, width, height, fps, audio, done);
        };
        send<void>(cls("SCShareableContent"),
                   "getShareableContentExcludingDesktopWindows:onScreenWindowsOnly:completionHandler:", NO, YES,
                   blockObject(handler));
    }

    void resize(int width, int height) override {
        std::scoped_lock l(m_);
        if (!stream_) return;
        send<void>(config_.get(), "setWidth:", size_t(width));
        send<void>(config_.get(), "setHeight:", size_t(height));
        send<void>(stream_.get(), "updateConfiguration:completionHandler:", config_.get(), nullptr);
    }

    void setShowsCursor(bool show) override {
        std::scoped_lock l(m_);
        if (!stream_ || show == showingCursor_) return;
        showingCursor_ = show;
        send<void>(config_.get(), "setShowsCursor:", BOOL(show));
        send<void>(stream_.get(), "updateConfiguration:completionHandler:", config_.get(), nullptr);
    }

    void stop(std::function<void()> done) override {
        Ref stream;
        {
            std::scoped_lock l(m_);
            stream = std::move(stream_);
            stream_ = Ref();
            last_.reset();
        }
        if (!stream) return done();
        void (^handler)(Obj) = ^(Obj) {
          done();
        };
        send<void>(stream.get(), "stopCaptureWithCompletionHandler:", blockObject(handler));
    }

    void resendLast() override {
        dispatch_async(video_, ^{
          std::optional<Frame> f;
          {
              std::scoped_lock l(m_);
              f = last_;
          }
          if (f && onFrame) {
              f->captured = Clock::now();
              onFrame(*f);
          }
        });
    }

private:
    void begin(Obj content, DisplayId displayId, int width, int height, int fps, bool audio,
               // NOLINTNEXTLINE(performance-unnecessary-value-param): the blocks below keep a copy
               std::function<void(const std::string&)> done) {
        Pool pool;
        Obj displays = send(content, "displays");
        Obj display = nullptr;
        for (unsigned long i = 0, n = send<unsigned long>(displays, "count"); i < n && !display; ++i) {
            Obj d = send(displays, "objectAtIndex:", i);
            if (send<uint32_t>(d, "displayID") == displayId) display = d;
        }
        if (!display) return done("display not shareable");
        Ref filter(send(send(cls("SCContentFilter"), "alloc"), "initWithDisplay:excludingWindows:", display,
                        send(cls("NSArray"), "array")));
        Ref config(make("SCStreamConfiguration"));
        send<void>(config.get(), "setWidth:", size_t(width));
        send<void>(config.get(), "setHeight:", size_t(height));
        send<void>(config.get(), "setMinimumFrameInterval:", CMTimeMake(1, fps));
        send<void>(config.get(), "setPixelFormat:", OSType(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange));
        send<void>(config.get(), "setQueueDepth:", long(5));
        send<void>(config.get(), "setShowsCursor:", NO); // setShowsCursor() turns it on when useful
        if (audio) {
            send<void>(config.get(), "setCapturesAudio:", YES);
            send<void>(config.get(), "setSampleRate:", long(48000));
            send<void>(config.get(), "setChannelCount:", long(2));
            send<void>(config.get(), "setExcludesCurrentProcessAudio:", YES);
        }
        Ref stream(send(send(cls("SCStream"), "alloc"), "initWithFilter:configuration:delegate:", filter.get(),
                        config.get(), delegate_.get()));
        Obj error = nullptr;
        send<BOOL>(stream.get(), "addStreamOutput:type:sampleHandlerQueue:error:", delegate_.get(), long(0),
                   reinterpret_cast<Obj>(video_), &error);
        if (audio)
            send<BOOL>(stream.get(), "addStreamOutput:type:sampleHandlerQueue:error:", delegate_.get(), long(1),
                       reinterpret_cast<Obj>(audio_), &error);
        {
            std::scoped_lock l(m_);
            stream_ = stream;
            config_ = config;
            showingCursor_ = false;
        }
        void (^started)(Obj) = ^(Obj e) {
          done(e ? toString(send(e, "localizedDescription")) : "");
        };
        send<void>(stream.get(), "startCaptureWithCompletionHandler:", blockObject(started));
    }

    void output(CMSampleBufferRef sb, long type) {
        if (!CMSampleBufferIsValid(sb)) return;
        if (type == 1) {
            if (auto pcm = interleavedInt16(sb); pcm && onAudio) onAudio(*pcm);
            return;
        }
        CFArrayRef atts = CMSampleBufferGetSampleAttachmentsArray(sb, false);
        if (!atts || CFArrayGetCount(atts) == 0) return;
        auto info = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(atts, 0));
        auto status = static_cast<CFNumberRef>(CFDictionaryGetValue(info, SCStreamFrameInfoStatus));
        int value = -1;
        if (!status || !CFNumberGetValue(status, kCFNumberIntType, &value) || value != 0) return; // 0: complete
        CVPixelBufferRef pb = CMSampleBufferGetImageBuffer(sb);
        if (!pb) return;
        CFRetain(pb);
        Frame f{std::shared_ptr<void>(pb, [](void* p) { CFRelease(p); }), int(CVPixelBufferGetWidth(pb)),
                int(CVPixelBufferGetHeight(pb)), Clock::now()};
        {
            std::scoped_lock l(m_);
            last_ = f;
        }
        if (onFrame) onFrame(f);
    }

    static Class delegateClass() {
        static Class c = [] {
            return ClassBuilder("SpanlyCaptureDelegate")
                .protocol("SCStreamOutput")
                .protocol("SCStreamDelegate")
                .method(
                    "stream:didOutputSampleBuffer:ofType:",
                    +[](Obj self, SEL, Obj, CMSampleBufferRef sb, long type) {
                        auto* a = owner<Anchor>(self);
                        std::scoped_lock l(a->m); // the capture can't go away meanwhile
                        if (a->capture) a->capture->output(sb, type);
                    },
                    "v@:@^vq")
                .method(
                    "stream:didStopWithError:",
                    +[](Obj self, SEL, Obj, Obj error) {
                        constexpr long kUserStopped = -3817; // SCStreamErrorUserStopped
                        long code = error ? send<long>(error, "code") : 0;
                        bool byUser = code == kUserStopped;
                        log("capture stopped: {} ({} {})", error ? toString(send(error, "localizedDescription")) : "",
                            error ? toString(send(error, "domain")) : "", code);
                        auto* a = owner<Anchor>(self);
                        std::scoped_lock l(a->m);
                        if (a->capture && a->capture->onStop) a->capture->onStop(byUser);
                    },
                    "v@:@@")
                .build();
        }();
        return c;
    }

    dispatch_queue_t video_;
    dispatch_queue_t audio_;
    Anchor* anchor_;
    Ref delegate_;
    std::mutex m_;
    Ref stream_;
    Ref config_;
    bool showingCursor_ = false;
    std::optional<Frame> last_;
};

} // namespace

std::unique_ptr<Capture> Capture::create() {
    return std::make_unique<MacCapture>();
}

} // namespace spanly::platform
