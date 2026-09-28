#pragma once
// The Spanly wire protocol (PROTOCOL.md). Every message, in both directions:
// [0x5A][type u8][length u32 BE][payload]. A reader only accepts a header whose marker matches
// and whose length is plausible for its type, so it can resynchronise after a reconnect
// (stale bytes can still be in the USB pipe).

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace spanly {

using Bytes = std::vector<uint8_t>;
using ByteView = std::span<const uint8_t>;

enum class Msg : uint8_t {
    Nop = 0, // heartbeat / padding, both directions
    // Host -> tablet
    Size = 1,
    Config = 2,
    Frame = 3,
    Display = 4,
    HelloRequest = 5,
    Audio = 6,
    Pair = 7,
    ShareStart = 8,
    ShareStop = 9,
    RemotePointer = 20,
    RemoteScroll = 21,
    RemoteKey = 22,
    MicStart = 26,
    MicStop = 27,
    UdpOffer = 29, // token (16) + UDP port u16: send your video registration there
    UdpReady = 30, // video now comes over UDP
    // Tablet -> host
    Touch = 10,
    Hello = 11,
    Ack = 12,
    Scroll = 13,
    Zoom = 14,
    Pen = 15,
    ShareSize = 16,
    ShareConfig = 17,
    ShareFrame = 18,
    Viewing = 19,
    ShareStatus = 23,
    ShareAudio = 24,
    MicAudio = 25,
    Standby = 28,
    KeyframeRequest = 31, // a frame was lost (video over UDP): send a keyframe
};

constexpr uint8_t kMarker = 0x5A;
constexpr size_t kHeader = 6;
constexpr size_t kMaxRecord = 20u << 20; // largest encrypted Wi-Fi record

void putU32(Bytes& out, uint32_t v);
uint32_t u32At(ByteView p, size_t i);
float f32At(ByteView p, size_t i);

Bytes encode(Msg type, ByteView payload = {});

/// Header sanity for messages the host receives.
bool plausible(uint8_t type, size_t length);

struct Message {
    uint8_t type = 0;
    Bytes payload;
};

/// Collects bytes from a link and yields complete messages, skipping anything that doesn't
/// start a plausible one.
class Reader {
public:
    void push(ByteView data);
    std::optional<Message> next();
    size_t skipped = 0; // bytes skipped to get back in sync since the caller last reset it

private:
    Bytes buf_;
    size_t start_ = 0;
};

/// A tablet's HELLO: its screen, decoder, and (newer apps) a stable ID telling tablets apart.
struct Hello {
    int w = 0, h = 0, dpi = 0;
    uint32_t caps = 0;      // bit 0: hardware HEVC
    int maxW = 0, maxH = 0; // largest size its hardware decoder handles at 60 fps (0 = unknown)
    std::string id;         // hex of the tablet's 16-byte ID ("" for older apps)
    std::string name;       // the tablet's name, when it fits after the ID

    static Hello parse(ByteView p); // p has at least 12 bytes (plausible() checked)
};

/// Splits an Annex-B stream into NAL units (without start codes).
std::vector<ByteView> nalUnits(ByteView s);

/// An H.264 access unit with its parameter sets (SPS, PPS) moved out, for encoders that put
/// them inline: CONFIG carries them, FRAME only the picture. Both stay Annex-B.
struct SplitAccessUnit {
    Bytes picture;
    std::optional<Bytes> config; // on keyframes
};
SplitAccessUnit splitParameterSets(ByteView annexB);

} // namespace spanly
