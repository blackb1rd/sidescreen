#include "core/protocol.hpp"
#include "test.hpp"

using namespace spanly;

static Bytes ack(uint32_t id) {
    Bytes p;
    putU32(p, id);
    return encode(Msg::Ack, p);
}

TEST(encodesMarkerTypeAndLength) {
    Bytes one{1};
    CHECK((encode(Msg::Display, one) == Bytes{0x5A, 4, 0, 0, 0, 1, 1}));
}

TEST(readsConsecutiveAndSplitMessages) {
    Reader r;
    Bytes whole = ack(7);
    Bytes second = ack(8);
    whole.insert(whole.end(), second.begin(), second.end());
    r.push(ByteView(whole.data(), 13)); // the first message plus part of the second
    auto a = r.next();
    CHECK(a && a->type == uint8_t(Msg::Ack) && u32At(a->payload, 0) == 7);
    CHECK(!r.next());
    r.push(ByteView(whole.data() + 13, whole.size() - 13));
    auto b = r.next();
    CHECK(b && u32At(b->payload, 0) == 8);
}

TEST(resynchronisesPastGarbage) {
    Reader r;
    Bytes junk{0x01, 0x5A, 0x0C, 0xFF, 0xFF, 0xFF, 0xFF, 0x33};
    r.push(junk);
    r.push(ack(5));
    auto m = r.next();
    CHECK(m && u32At(m->payload, 0) == 5);
    CHECK(r.skipped == 8);
}

TEST(rejectsImplausibleHeaders) {
    CHECK(plausible(uint8_t(Msg::Ack), 4));
    CHECK(!plausible(uint8_t(Msg::Ack), 5));
    CHECK(!plausible(uint8_t(Msg::Frame), 100)); // host -> tablet only
    CHECK(plausible(uint8_t(Msg::Standby), 16));
    CHECK(!plausible(uint8_t(Msg::MicAudio), 3));
}

TEST(parsesHelloWithTabletIdAndName) {
    Bytes p;
    for (uint32_t v : {1600u, 2560u, 360u, 1u, 2304u, 1440u})
        putU32(p, v);
    for (uint8_t i = 0; i < 16; ++i)
        p.push_back(i);
    for (char c : std::string("Redmi Pad 2"))
        p.push_back(uint8_t(c));
    Hello h = Hello::parse(p);
    CHECK(h.w == 1600 && h.h == 2560 && h.dpi == 360 && h.caps == 1 && h.maxW == 2304);
    CHECK(h.id == "000102030405060708090a0b0c0d0e0f");
    CHECK(h.name == "Redmi Pad 2");
}

TEST(splitsAnnexB) {
    Bytes s{0, 0, 0, 1, 0x67, 1, 2, 0, 0, 1, 0x68, 3, 0, 0, 0, 1, 0x65, 4, 5};
    auto nals = nalUnits(s);
    CHECK(nals.size() == 3);
    CHECK(nals[0].size() == 3 && nals[0][0] == 0x67);
    CHECK(nals[1].size() == 2 && nals[2][2] == 5);
}
