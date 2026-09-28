#include "core/crypto.hpp"

#include <mbedtls/gcm.h>
#include <mbedtls/hkdf.h>
#include <mbedtls/md.h>

#include <stdexcept>

#ifdef _WIN32
#include <windows.h>
// (after windows.h, which defines the types it uses)
#include <bcrypt.h>
#else
#include <unistd.h>
#if defined(__APPLE__)
#include <sys/random.h>
#endif
#endif

namespace spanly::crypto {

void randomBytes(uint8_t* out, size_t n) {
#ifdef _WIN32
    if (BCryptGenRandom(nullptr, out, ULONG(n), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
        throw std::runtime_error("BCryptGenRandom failed");
#else
    while (n > 0) {
        size_t chunk = n < 256 ? n : 256; // getentropy's limit
        if (getentropy(out, chunk) != 0) throw std::runtime_error("getentropy failed");
        out += chunk;
        n -= chunk;
    }
#endif
}

Bytes random(size_t n) {
    Bytes b(n);
    randomBytes(b.data(), n);
    return b;
}

Key hkdf(ByteView secret, ByteView salt, std::string_view info) {
    Key k{};
    mbedtls_hkdf(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), salt.data(), salt.size(), secret.data(), secret.size(),
                 reinterpret_cast<const unsigned char*>(info.data()), info.size(), k.data(), k.size());
    return k;
}

Keys serverKeys(ByteView secret, ByteView clientNonce, ByteView serverNonce) {
    Bytes salt(clientNonce.begin(), clientNonce.end());
    salt.insert(salt.end(), serverNonce.begin(), serverNonce.end());
    return {hkdf(secret, salt, "spanly s2c"), hkdf(secret, salt, "spanly c2s")};
}

Key udpKey(ByteView secret, ByteView clientNonce, ByteView serverNonce) {
    Bytes salt(clientNonce.begin(), clientNonce.end());
    salt.insert(salt.end(), serverNonce.begin(), serverNonce.end());
    return hkdf(secret, salt, "spanly udp s2c");
}

namespace {

std::array<uint8_t, 12> nonce(uint64_t counter) {
    std::array<uint8_t, 12> n{};
    for (unsigned i = 0; i < 8; ++i)
        n[4 + i] = uint8_t(counter >> (56U - 8U * i));
    return n;
}

struct Gcm {
    mbedtls_gcm_context ctx;
    explicit Gcm(const Key& key) {
        mbedtls_gcm_init(&ctx);
        mbedtls_gcm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, key.data(), 256);
    }
    ~Gcm() { mbedtls_gcm_free(&ctx); }
};

} // namespace

Bytes seal(ByteView plain, const Key& key, uint64_t counter) {
    Gcm g(key);
    auto iv = nonce(counter);
    Bytes out(plain.size() + kTagSize);
    mbedtls_gcm_crypt_and_tag(&g.ctx, MBEDTLS_GCM_ENCRYPT, plain.size(), iv.data(), iv.size(), nullptr, 0, plain.data(),
                              out.data(), kTagSize, out.data() + plain.size());
    return out;
}

std::optional<Bytes> open(ByteView record, const Key& key, uint64_t counter) {
    if (record.size() < kTagSize) return std::nullopt;
    size_t n = record.size() - kTagSize;
    Gcm g(key);
    auto iv = nonce(counter);
    Bytes out(n);
    if (mbedtls_gcm_auth_decrypt(&g.ctx, n, iv.data(), iv.size(), nullptr, 0, record.data() + n, kTagSize,
                                 record.data(), out.data()) != 0)
        return std::nullopt;
    return out;
}

} // namespace spanly::crypto
