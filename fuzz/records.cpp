// Encrypted Wi-Fi records, the first thing that reads bytes from another device on the network.
// Mode 0: the input as the raw stream (lengths, truncation, authentication failures).
// Mode 1: the input cut into messages that are properly sealed, then split again as a network
// would, so what's inside the records reaches the Reader and the message handlers too.
#include "core/records.hpp"
#include "fuzz_link.hpp"

using namespace spanly;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    static auto link = std::make_shared<fuzz::FuzzLink>();
    if (size < 2) return 0;
    const crypto::Key key{1, 2, 3};
    bool sealed = (data[0] & 1U) != 0;
    size_t step = size_t(data[1]) + 1;
    ByteView input(data + 2, size - 2);

    Bytes stream;
    if (!sealed) {
        stream.assign(input.begin(), input.end());
    } else {
        RecordSealer sealer(key);
        // Each message: a length byte, then that many bytes (or whatever is left).
        for (size_t off = 0; off < input.size();) {
            size_t len = std::min<size_t>(input[off], input.size() - off - 1);
            Bytes record = sealer.seal(input.subspan(off + 1, len));
            stream.insert(stream.end(), record.begin(), record.end());
            off += 1 + len;
        }
    }

    RecordOpener opener(key);
    Reader reader;
    Bytes raw;
    for (size_t off = 0; off < stream.size(); off += step) {
        size_t n = std::min(step, stream.size() - off);
        raw.insert(raw.end(), stream.begin() + ptrdiff_t(off), stream.begin() + ptrdiff_t(off + n));
        bool ok = opener.open(raw, reader);
        if (sealed && !ok) __builtin_trap(); // records we sealed ourselves must open
        link->drainAll(reader);
        if (!ok) break;
    }
    return 0;
}
