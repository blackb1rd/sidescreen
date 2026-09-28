#include "core/records.hpp"

namespace spanly {

Bytes RecordSealer::seal(ByteView plain) {
    Bytes sealed = crypto::seal(plain, key_, counter_++);
    Bytes record;
    record.reserve(4 + sealed.size());
    putU32(record, uint32_t(sealed.size()));
    record.insert(record.end(), sealed.begin(), sealed.end());
    return record;
}

bool RecordOpener::open(Bytes& raw, Reader& out) {
    size_t off = 0;
    bool ok = true;
    while (raw.size() - off >= 4) {
        size_t len = u32At(raw, off);
        if (len > kMaxRecord) {
            ok = false;
            break;
        }
        if (raw.size() - off - 4 < len) break;
        auto plain = crypto::open(ByteView(raw.data() + off + 4, len), key_, counter_++);
        if (!plain) {
            ok = false;
            break;
        }
        out.push(*plain);
        off += 4 + len;
    }
    raw.erase(raw.begin(), raw.begin() + ptrdiff_t(off));
    return ok;
}

} // namespace spanly
