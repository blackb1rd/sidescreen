#pragma once
// The Linux GPU path: VA-API (Intel, AMD). Capture converts on the GPU (vapostproc) and the
// encoder (vah264lpenc / vah264enc) takes those frames without a copy, on the same VA display.

#include <gst/gst.h>

namespace spanly::platform::va {

/// VA-API conversion and encoding are both installed (and SPANLY_NO_VA isn't set).
bool available();
/// The best VA-API H.264 encoder element name.
const char* encoderName();
/// The capture's VA display, handed to the encoder so both use the same one.
void setDisplayContext(GstContext* context); // takes a reference
GstContext* displayContext();                // a new reference, or null

} // namespace spanly::platform::va
