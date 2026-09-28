// H.264 streams the tablet sends when it shares its screen (the Mac shows them with
// VideoToolbox after splitting them into NAL units), and the parameter-set split used on the
// encoders' output.
#include "core/protocol.hpp"

using namespace spanly;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    ByteView s(data, size);
    size_t total = 0;
    for (ByteView nal : nalUnits(s)) {
        if (nal.data() < data || nal.data() + nal.size() > data + size) __builtin_trap();
        total += nal.size();
    }
    if (total > size) __builtin_trap();
    auto split = splitParameterSets(s);
    if (split.config && split.config->empty()) __builtin_trap();
    return 0;
}
