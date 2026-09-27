// H.264 encoding on Linux with GStreamer, preferring hardware encoders (VA-API, NVENC) and
// falling back to software (x264, OpenH264). Low latency: no B-frames, constant bitrate,
// keyframes on request.
#include "core/log.hpp"
#include "platform/platform.hpp"

#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/gst.h>
#include <gst/video/video.h>

#include <atomic>
#include <deque>
#include <format>
#include <mutex>

namespace spanly::platform {

namespace {

/// In order of preference; and whether the bitrate property is in kbit/s.
constexpr std::pair<const char*, bool> kEncoders[] = {
    {"vah264lpenc", true},  // VA-API low-power (Intel, AMD)
    {"vah264enc", true},    // VA-API
    {"vaapih264enc", true}, // older VA-API plugin
    {"nvh264enc", true},    // NVIDIA NVENC
    {"x264enc", true},      // software
    {"openh264enc", false}, // software, bit/s
};

void setIfPresent(GstElement* e, const char* property, const std::string& value) {
    if (g_object_class_find_property(G_OBJECT_GET_CLASS(e), property))
        gst_util_set_object_arg(G_OBJECT(e), property, value.c_str());
}

class GstEncoder : public Encoder {
public:
    GstEncoder(GstElement* pipeline, GstElement* src, GstElement* enc, bool kbps, int w, int h)
        : pipeline_(pipeline), src_(src), enc_(enc), kbps_(kbps), w_(w), h_(h) {
        GstElement* sink = gst_bin_get_by_name(GST_BIN(pipeline_), "sink");
        GstAppSinkCallbacks callbacks{};
        callbacks.new_sample = [](GstAppSink* sink, gpointer self) {
            static_cast<GstEncoder*>(self)->output(gst_app_sink_pull_sample(sink));
            return GST_FLOW_OK;
        };
        gst_app_sink_set_callbacks(GST_APP_SINK(sink), &callbacks, this, nullptr);
        gst_object_unref(sink);
        gst_element_set_state(pipeline_, GST_STATE_PLAYING);
    }

    ~GstEncoder() override {
        gst_element_set_state(pipeline_, GST_STATE_NULL);
        gst_object_unref(src_);
        gst_object_unref(enc_);
        gst_object_unref(pipeline_);
    }

    Codec codec() const override { return Codec::H264; }
    int width() const override { return w_; }
    int height() const override { return h_; }

    void encode(const Frame& frame) override {
        if (frame.width != w_ || frame.height != h_) return; // a resize is settling
        auto* sample = static_cast<GstSample*>(frame.image.get());
        GstBuffer* buffer = gst_buffer_copy(gst_sample_get_buffer(sample)); // shares the memory
        GST_BUFFER_PTS(buffer) = GST_BUFFER_DTS(buffer) = GST_CLOCK_TIME_NONE;
        {
            std::scoped_lock l(m_);
            captured_.push_back(frame.captured);
            if (captured_.size() > 16) captured_.pop_front();
        }
        if (forceKey_.exchange(false)) {
            gst_element_send_event(src_, gst_video_event_new_downstream_force_key_unit(
                                             GST_CLOCK_TIME_NONE, GST_CLOCK_TIME_NONE, GST_CLOCK_TIME_NONE, TRUE, 0));
        }
        gst_app_src_push_buffer(GST_APP_SRC(src_), buffer);
    }

    void requestKeyframe() override { forceKey_ = true; }

    void setBitrate(int bps) override { setIfPresent(enc_, "bitrate", std::to_string(kbps_ ? bps / 1000 : bps)); }

private:
    void output(GstSample* s) {
        if (!s) return;
        GstBuffer* b = gst_sample_get_buffer(s);
        GstMapInfo map;
        if (b && gst_buffer_map(b, &map, GST_MAP_READ)) {
            bool key = !GST_BUFFER_FLAG_IS_SET(b, GST_BUFFER_FLAG_DELTA_UNIT);
            auto split = splitParameterSets(ByteView(map.data, map.size));
            gst_buffer_unmap(b, &map);
            Clock::time_point captured = Clock::now();
            {
                std::scoped_lock l(m_);
                if (!captured_.empty()) {
                    captured = captured_.front();
                    captured_.pop_front();
                }
            }
            if (onFrame && !split.picture.empty())
                onFrame(std::move(split.picture), key, key ? std::move(split.config) : std::nullopt, captured);
        }
        gst_sample_unref(s);
    }

    GstElement* pipeline_;
    GstElement* src_;
    GstElement* enc_;
    bool kbps_;
    int w_, h_;
    std::atomic<bool> forceKey_{true};
    std::mutex m_;
    std::deque<Clock::time_point> captured_;
};

} // namespace

std::unique_ptr<Encoder> Encoder::create(int width, int height, int fps, int bitrate, Codec codec) {
    if (codec != Codec::H264) return nullptr; // HEVC: H.264 is what every Linux setup can encode
    gst_init(nullptr, nullptr);
    const std::pair<const char*, bool>* found = nullptr;
    for (const auto& e : kEncoders) {
        if (GstElementFactory* f = gst_element_factory_find(e.first)) {
            gst_object_unref(f);
            found = &e;
            break;
        }
    }
    if (!found) {
        log("no H.264 encoder found: install gstreamer1.0-plugins-bad (VA-API) or -ugly (x264)");
        return nullptr;
    }
    std::string desc =
        std::format("appsrc name=src is-live=true format=time do-timestamp=true "
                    "caps=video/x-raw,format=NV12,width={},height={},framerate={}/1 ! {} name=enc ! "
                    "h264parse config-interval=-1 ! video/x-h264,stream-format=byte-stream,alignment=au ! "
                    "appsink name=sink sync=false max-buffers=4 drop=false",
                    width, height, fps, found->first);
    GError* error = nullptr;
    GstElement* pipeline = gst_parse_launch(desc.c_str(), &error);
    if (!pipeline) {
        log("H.264 encoder: {}", error ? error->message : "pipeline failed");
        if (error) g_error_free(error);
        return nullptr;
    }
    GstElement* enc = gst_bin_get_by_name(GST_BIN(pipeline), "enc");
    // Low latency: no B-frames, constant bitrate, a keyframe only every 10 s (plus on request).
    setIfPresent(enc, "bitrate", std::to_string(found->second ? bitrate / 1000 : bitrate));
    setIfPresent(enc, "rate-control", "cbr");
    for (const char* p : {"b-frames", "bframes", "max-bframes"})
        setIfPresent(enc, p, "0");
    for (const char* p : {"key-int-max", "gop-size"})
        setIfPresent(enc, p, std::to_string(fps * 10));
    setIfPresent(enc, "tune", "zerolatency");
    setIfPresent(enc, "speed-preset", "ultrafast");
    setIfPresent(enc, "preset", "low-latency-hp");
    setIfPresent(enc, "target-usage", "7");
    log("H.264 encoder: {}", found->first);
    return std::make_unique<GstEncoder>(pipeline, gst_bin_get_by_name(GST_BIN(pipeline), "src"), enc, found->second,
                                        width, height);
}

} // namespace spanly::platform
