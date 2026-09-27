// Sound on macOS: muting the speakers (CoreAudio), playing PCM (AudioQueue), and installing the
// "Spanly Microphone" driver.
#include "core/log.hpp"
#include "platform/mac/objc.hpp"
#include "platform/platform.hpp"

#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>

#include <algorithm>
#include <cstring>
#include <deque>
#include <mutex>

namespace spanly::platform {

using namespace mac;

namespace {

AudioObjectID defaultOutput() {
    AudioObjectID id = kAudioObjectUnknown;
    UInt32 size = sizeof id;
    AudioObjectPropertyAddress a{kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal,
                                 kAudioObjectPropertyElementMain};
    AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, nullptr, &size, &id);
    return id;
}

constexpr AudioObjectPropertyAddress kMute{kAudioDevicePropertyMute, kAudioDevicePropertyScopeOutput,
                                           kAudioObjectPropertyElementMain};

bool muted() {
    UInt32 v = 0, size = sizeof v;
    AudioObjectID d = defaultOutput();
    return d != kAudioObjectUnknown && AudioObjectGetPropertyData(d, &kMute, 0, nullptr, &size, &v) == noErr && v != 0;
}

void setMuted(bool on) {
    UInt32 v = on ? 1 : 0;
    if (AudioObjectID d = defaultOutput(); d != kAudioObjectUnknown)
        AudioObjectSetPropertyData(d, &kMute, 0, nullptr, sizeof v, &v);
}

AudioObjectID deviceForUid(const std::string& uid) {
    CFStringRef cf = CFStringCreateWithCString(nullptr, uid.c_str(), kCFStringEncodingUTF8);
    AudioObjectID device = kAudioObjectUnknown;
    UInt32 size = sizeof device;
    AudioObjectPropertyAddress a{kAudioHardwarePropertyTranslateUIDToDevice, kAudioObjectPropertyScopeGlobal,
                                 kAudioObjectPropertyElementMain};
    AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, sizeof cf, &cf, &size, &device);
    CFRelease(cf);
    return device;
}

constexpr const char* kMutedKey = "mutedMacSpeakers";

} // namespace

void muteSpeakers() {
    auto& store = Store::shared();
    if (store.boolean(kMutedKey).value_or(false) || muted()) return;
    store.set(kMutedKey, true);
    setMuted(true);
    log("speakers muted: sound plays on the tablet only");
}

void restoreSpeakers() {
    auto& store = Store::shared();
    if (!store.boolean(kMutedKey).value_or(false)) return;
    store.set(kMutedKey, false);
    setMuted(false);
    log("speakers restored");
}

namespace {

class MacPcmPlayer : public PcmPlayer {
public:
    static constexpr int kRate = 48000;
    static constexpr size_t kBufferFrames = 480; // 10 ms per queue buffer
    static constexpr size_t kMaxQueued = kRate * 150 / 1000;

    explicit MacPcmPlayer(int channels) : channels_(size_t(channels)) {}

    bool start(const std::string& uid) {
        AudioStreamBasicDescription f{};
        f.mSampleRate = kRate;
        f.mFormatID = kAudioFormatLinearPCM;
        f.mFormatFlags = kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked;
        f.mChannelsPerFrame = UInt32(channels_);
        f.mBitsPerChannel = 16;
        f.mBytesPerFrame = f.mBytesPerPacket = UInt32(2 * channels_);
        f.mFramesPerPacket = 1;
        if (AudioQueueNewOutput(&f, callback, this, nullptr, nullptr, 0, &queue_) != noErr) return false;
        if (!uid.empty()) {
            CFStringRef cf = CFStringCreateWithCString(nullptr, uid.c_str(), kCFStringEncodingUTF8);
            OSStatus s = AudioQueueSetProperty(queue_, kAudioQueueProperty_CurrentDevice, &cf, sizeof cf);
            CFRelease(cf);
            if (s != noErr) return false;
        }
        for (int i = 0; i < 3; ++i) {
            AudioQueueBufferRef b = nullptr;
            AudioQueueAllocateBuffer(queue_, UInt32(kBufferFrames * 2 * channels_), &b);
            fill(b);
        }
        return AudioQueueStart(queue_, nullptr) == noErr;
    }

    ~MacPcmPlayer() override {
        if (queue_) AudioQueueDispose(queue_, true);
    }

    void play(const Bytes& pcm) override {
        size_t n = pcm.size() / 2;
        std::scoped_lock l(m_);
        if (samples_.size() / channels_ > kMaxQueued) return; // behind: drop rather than drift
        const auto* s = reinterpret_cast<const int16_t*>(pcm.data());
        samples_.insert(samples_.end(), s, s + n);
    }

private:
    static void callback(void* self, AudioQueueRef, AudioQueueBufferRef b) {
        static_cast<MacPcmPlayer*>(self)->fill(b);
    }

    /// Next 10 ms: queued sound, then silence if the tablet is behind.
    void fill(AudioQueueBufferRef b) {
        auto* out = static_cast<int16_t*>(b->mAudioData);
        size_t want = kBufferFrames * channels_;
        {
            std::scoped_lock l(m_);
            size_t n = std::min(want, samples_.size());
            std::copy_n(samples_.begin(), n, out);
            samples_.erase(samples_.begin(), samples_.begin() + ptrdiff_t(n));
            std::fill(out + n, out + want, int16_t(0));
        }
        b->mAudioDataByteSize = UInt32(want * 2);
        AudioQueueEnqueueBuffer(queue_, b, 0, nullptr);
    }

    size_t channels_;
    AudioQueueRef queue_ = nullptr;
    std::mutex m_;
    std::deque<int16_t> samples_;
};

} // namespace

std::unique_ptr<PcmPlayer> PcmPlayer::create(int channels, const std::string& deviceUid) {
    if (!deviceUid.empty() && deviceForUid(deviceUid) == kAudioObjectUnknown) return nullptr;
    auto p = std::make_unique<MacPcmPlayer>(channels);
    if (!p->start(deviceUid)) {
        log("audio playback could not start");
        return nullptr;
    }
    return p;
}

bool microphoneInstalled() {
    return deviceForUid(kMicrophoneUid) != kAudioObjectUnknown;
}

/// Copy the driver from the app into place and restart the audio service (admin password).
void installMicrophone() {
    Pool pool;
    Obj bundle = send(cls("NSBundle"), "mainBundle");
    std::string driver = toString(send(bundle, "resourcePath")) + "/SpanlyMicrophone.driver";
    const std::string dir = "/Library/Audio/Plug-Ins/HAL";
    auto quote = [](std::string s) {
        std::string q = "'";
        for (char c : s)
            q += c == '\'' ? std::string("'\\''") : std::string(1, c);
        return q + "'";
    };
    // Also removes the driver of the app's earlier name.
    std::string command = "mkdir -p " + dir + " && rm -rf " + dir + "/SpanlyMicrophone.driver " + dir +
                          "/SideScreenMicrophone.driver && cp -R " + quote(driver) + " " + dir +
                          "/ && killall coreaudiod";
    std::string escaped;
    for (char c : command) {
        if (c == '\\' || c == '"') escaped += '\\';
        escaped += c;
    }
    std::string script = "do shell script \"" + escaped + "\" with administrator privileges";
    Ref apple(send(send(cls("NSAppleScript"), "alloc"), "initWithSource:", nsString(script)));
    Obj error = nullptr;
    send(apple.get(), "executeAndReturnError:", &error);
    log("{}", error ? "the microphone driver was not installed" : "installed the Spanly Microphone driver");
}

} // namespace spanly::platform
