// Capture on Linux: GStreamer reads the portal's PipeWire stream and turns it into NV12 at the
// stream size: on the GPU with VA-API (frames stay in GPU memory, see va.hpp), else on the CPU.
// Frames are GstSamples (the encoder takes the buffer without copying).
#include "core/log.hpp"
#include "platform/linux/portal.hpp"
#include "platform/linux/va.hpp"
#include "platform/platform.hpp"

#include <gst/app/gstappsink.h>
#include <gst/gst.h>

#include <format>
#include <mutex>
#include <unistd.h>

namespace spanly::platform {

namespace {

class GstCapture : public Capture {
public:
    GstCapture() { gst_init(nullptr, nullptr); }
    ~GstCapture() override { teardown(); }

    void start(DisplayId id, int width, int height, int fps, bool,
               std::function<void(const std::string&)> done) override {
        teardown();
        auto* s = portal::sessionFor(id);
        if (!s) return done("no desktop portal stream for this display");
        GError* error = nullptr;
        gpu_ = va::available();
        contextSaved_ = false;
        // The first caps ask the compositor for a virtual monitor of the tablet's size.
        std::string desc = std::format(
            "pipewiresrc name=src do-timestamp=true keepalive-time=1000 ! capsfilter name=want ! {} ! "
            "capsfilter name=raw ! videorate max-rate={} drop-only=true ! appsink name=sink sync=false max-buffers=2 "
            "drop=true",
            gpu_ ? "vapostproc name=post" : "videoconvert ! videoscale", fps);
        pipeline_ = gst_parse_launch(desc.c_str(), &error);
        if (!pipeline_) {
            std::string e = error ? error->message : "GStreamer pipeline";
            if (error) g_error_free(error);
            return done(e + " (install the GStreamer PipeWire plugin, gstreamer1.0-pipewire)");
        }
        GstElement* src = gst_bin_get_by_name(GST_BIN(pipeline_), "src");
        g_object_set(src, "fd", dup(s->pipeWireFd()), "path", std::to_string(s->node()).c_str(), nullptr);
        gst_object_unref(src);
        setSize(width, height);
        GstElement* sink = gst_bin_get_by_name(GST_BIN(pipeline_), "sink");
        GstAppSinkCallbacks callbacks{};
        callbacks.new_sample = [](GstAppSink* sink, gpointer self) {
            static_cast<GstCapture*>(self)->sample(gst_app_sink_pull_sample(sink));
            return GST_FLOW_OK;
        };
        gst_app_sink_set_callbacks(GST_APP_SINK(sink), &callbacks, this, nullptr);
        gst_object_unref(sink);
        GstBus* bus = gst_element_get_bus(pipeline_);
        gst_bus_add_watch(
            bus,
            [](GstBus*, GstMessage* m, gpointer self) -> gboolean {
                if (GST_MESSAGE_TYPE(m) == GST_MESSAGE_ERROR) {
                    GError* e = nullptr;
                    gst_message_parse_error(m, &e, nullptr);
                    log("capture stopped: {}", e ? e->message : "?");
                    if (e) g_error_free(e);
                    auto* c = static_cast<GstCapture*>(self);
                    if (c->onStop) c->onStop();
                }
                return TRUE;
            },
            this);
        gst_object_unref(bus);
        bool ok = gst_element_set_state(pipeline_, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE;
        done(ok ? "" : "could not start the capture pipeline");
    }

    void resize(int width, int height) override { setSize(width, height); }
    void setShowsCursor(bool) override {} // the portal draws the cursor into the stream

    void stop(std::function<void()> done) override {
        teardown();
        done();
    }

    void resendLast() override {
        std::optional<Frame> f;
        {
            std::scoped_lock l(m_);
            f = last_;
        }
        if (f && onFrame) {
            f->captured = Clock::now();
            onFrame(*f);
        }
    }

private:
    void setSize(int width, int height) {
        if (!pipeline_) return;
        for (const char* name : {"want", "raw"}) {
            GstElement* filter = gst_bin_get_by_name(GST_BIN(pipeline_), name);
            bool raw = std::string(name) == "raw";
            GstCaps* caps = raw ? gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING, "NV12", "width",
                                                      G_TYPE_INT, width, "height", G_TYPE_INT, height, nullptr)
                                : gst_caps_new_simple("video/x-raw", "width", G_TYPE_INT, width, "height", G_TYPE_INT,
                                                      height, nullptr);
            if (raw && gpu_) gst_caps_set_features(caps, 0, gst_caps_features_new("memory:VAMemory", nullptr));
            g_object_set(filter, "caps", caps, nullptr);
            gst_caps_unref(caps);
            gst_object_unref(filter);
        }
    }

    void sample(GstSample* s) {
        if (!s) return;
        if (gpu_ && !contextSaved_) { // the encoder must use the same VA display
            GstElement* post = gst_bin_get_by_name(GST_BIN(pipeline_), "post");
            if (GstContext* ctx = post ? gst_element_get_context(post, "gst.va.display.handle") : nullptr) {
                va::setDisplayContext(ctx);
                gst_context_unref(ctx);
                contextSaved_ = true;
            }
            if (post) gst_object_unref(post);
        }
        GstCaps* caps = gst_sample_get_caps(s);
        GstStructure* st = caps ? gst_caps_get_structure(caps, 0) : nullptr;
        int w = 0, h = 0;
        if (st) {
            gst_structure_get_int(st, "width", &w);
            gst_structure_get_int(st, "height", &h);
        }
        Frame f{std::shared_ptr<void>(s, [](void* p) { gst_sample_unref(static_cast<GstSample*>(p)); }), w, h,
                Clock::now()};
        {
            std::scoped_lock l(m_);
            last_ = f;
        }
        if (onFrame) onFrame(f);
    }

    void teardown() {
        if (!pipeline_) return;
        gst_element_set_state(pipeline_, GST_STATE_NULL);
        gst_object_unref(pipeline_);
        pipeline_ = nullptr;
        std::scoped_lock l(m_);
        last_.reset();
    }

    GstElement* pipeline_ = nullptr;
    bool gpu_ = false;
    bool contextSaved_ = false; // capture thread
    std::mutex m_;
    std::optional<Frame> last_;
};

} // namespace

std::unique_ptr<Capture> Capture::create() {
    return std::make_unique<GstCapture>();
}

} // namespace spanly::platform
