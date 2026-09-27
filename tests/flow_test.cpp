#include "core/flow.hpp"
#include "test.hpp"

using namespace spanly;

TEST(limitsFramesInFlightUntilAcked) {
    FlowControl f;
    f.reset(3);
    for (int i = 0; i < 3; ++i) {
        CHECK(!f.tooManyInFlight());
        f.registerFrame(Clock::now());
    }
    CHECK(f.tooManyInFlight());
    CHECK(f.acked(2).has_value());
    CHECK(!f.tooManyInFlight());
    CHECK(!f.acked(1)); // older than the last ACK
}

TEST(adaptsTheBitrate) {
    FlowControl::Window bad{20, 120, 10};
    CHECK(adaptBitrate(20, 20, bad) == 15.0);
    FlowControl::Window good{60, 30, 0};
    auto up = adaptBitrate(10, 20, good);
    CHECK(up && *up > 10.99 && *up < 11.01);
    CHECK(!adaptBitrate(20, 20, good)); // already at the ceiling
}
