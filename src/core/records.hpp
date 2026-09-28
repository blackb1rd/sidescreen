#pragma once
// Encrypted Wi-Fi records (PROTOCOL.md, "Wi-Fi"): length u32 BE + AES-256-GCM(ciphertext || tag),
// with a per-direction counter as the nonce. Kept apart from the socket code so it can be tested
// and fuzzed on its own.

#include "core/crypto.hpp"

namespace spanly {

/// Seals messages into records, one direction.
class RecordSealer {
public:
    explicit RecordSealer(const crypto::Key& key) : key_(key) {}
    Bytes seal(ByteView plain);

private:
    crypto::Key key_;
    uint64_t counter_ = 0;
};

/// Opens records from a byte stream, one direction.
class RecordOpener {
public:
    explicit RecordOpener(const crypto::Key& key) : key_(key) {}
    /// Opens every complete record at the front of `raw` (and removes it), passing the plaintext
    /// to `out`. False if a record is too long or fails authentication: the connection is bad.
    bool open(Bytes& raw, Reader& out);

private:
    crypto::Key key_;
    uint64_t counter_ = 0;
};

} // namespace spanly
