#pragma once
// Encryption for the Wi-Fi link (PROTOCOL.md, "Wi-Fi"). The tablet receives a 32-byte pairing
// secret over USB. Each Wi-Fi session starts with both sides exchanging a random 16-byte nonce;
// HKDF-SHA256 turns secret + nonces into one key per direction. Every record is AES-256-GCM
// with a per-direction counter as the nonce (4 zero bytes + big-endian u64).

#include "core/protocol.hpp"

#include <array>
#include <optional>
#include <string_view>

namespace spanly::crypto {

using Key = std::array<uint8_t, 32>;

constexpr std::string_view kMagic = "SPW1"; // the tablet's first bytes on a Wi-Fi connection
constexpr size_t kNonceSize = 16;
constexpr size_t kTagSize = 16;

struct Keys {
    Key send;
    Key receive;
};

void randomBytes(uint8_t* out, size_t n);
Bytes random(size_t n);

Key hkdf(ByteView secret, ByteView salt, std::string_view info);

/// Session keys for the host (server) side.
Keys serverKeys(ByteView secret, ByteView clientNonce, ByteView serverNonce);

/// ciphertext || tag
Bytes seal(ByteView plain, const Key& key, uint64_t counter);
std::optional<Bytes> open(ByteView record, const Key& key, uint64_t counter);

} // namespace spanly::crypto
