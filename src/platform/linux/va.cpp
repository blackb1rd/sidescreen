#include "platform/linux/va.hpp"

#include <cstdlib>
#include <mutex>

namespace spanly::platform::va {

namespace {
std::mutex gLock;
GstContext* gContext = nullptr;

bool has(const char* element) {
    GstElementFactory* f = gst_element_factory_find(element);
    if (f) gst_object_unref(f);
    return f != nullptr;
}
} // namespace

bool available() {
    gst_init(nullptr, nullptr);
    return !std::getenv("SPANLY_NO_VA") && has("vapostproc") && encoderName() != nullptr;
}

const char* encoderName() {
    for (const char* e : {"vah264lpenc", "vah264enc"}) {
        if (has(e)) return e;
    }
    return nullptr;
}

void setDisplayContext(GstContext* context) {
    std::scoped_lock l(gLock);
    if (gContext) gst_context_unref(gContext);
    gContext = context ? gst_context_ref(context) : nullptr;
}

GstContext* displayContext() {
    std::scoped_lock l(gLock);
    return gContext ? gst_context_ref(gContext) : nullptr;
}

} // namespace spanly::platform::va
