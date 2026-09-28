// Bytes from a tablet (USB, adb, or decrypted Wi-Fi records) through the Reader and every message
// handler. The first byte picks how the rest is split into pushes, as a network would split it.
#include "fuzz_link.hpp"

using namespace spanly;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    static auto link = std::make_shared<fuzz::FuzzLink>();
    if (size == 0) return 0;
    size_t step = size_t(data[0]) + 1;
    Reader reader;
    for (size_t off = 1; off < size; off += step) {
        reader.push(ByteView(data + off, std::min(step, size - off)));
        link->drainAll(reader);
    }
    return 0;
}
