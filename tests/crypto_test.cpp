#include "core/crypto.hpp"
#include "test.hpp"

using namespace spanly;

// The same vectors are checked by the Android app (SecureChannelTest).
static void vectors(Bytes& secret, Bytes& cn, Bytes& sn) {
    for (uint8_t i = 1; i <= 32; ++i)
        secret.push_back(i);
    for (uint8_t i = 0; i < 16; ++i) {
        cn.push_back(0xA0 + i);
        sn.push_back(0xB0 + i);
    }
}

TEST(derivesTheSameKeysAsTheTablet) {
    Bytes secret, cn, sn;
    vectors(secret, cn, sn);
    auto keys = crypto::serverKeys(secret, cn, sn);
    CHECK(test::hex(keys.receive.data(), 32) == "7b00117bc521fd6b60ab168938a85eb36ed13daf2412cf3e81de7430c27ebf00");
    CHECK(test::hex(keys.send.data(), 32) == "870ed19deb24707f0404ce34e8bb337eaeca4c7422884fe35faa23d1b05cbb57");
}

TEST(sealsLikeTheTablet) {
    Bytes secret, cn, sn;
    vectors(secret, cn, sn);
    auto keys = crypto::serverKeys(secret, cn, sn);
    std::string text = "hello tablet";
    Bytes sealed = crypto::seal(Bytes(text.begin(), text.end()), keys.send, 5);
    CHECK(test::hex(sealed.data(), sealed.size()) == "0abf9ce6e9fcb34d91cbf7019062ffe0b45bd93794ff6d3b1b1b9eb8");
}

TEST(roundTripsAndRejectsTampering) {
    crypto::Key key{};
    crypto::randomBytes(key.data(), key.size());
    Bytes plain{1, 2, 3, 4, 5};
    Bytes sealed = crypto::seal(plain, key, 7);
    CHECK(crypto::open(sealed, key, 7) == plain);
    CHECK(!crypto::open(sealed, key, 8)); // wrong counter
    sealed[0] ^= 1;
    CHECK(!crypto::open(sealed, key, 7));
}
