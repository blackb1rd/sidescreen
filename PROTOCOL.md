# SideScreen protocol

How the Mac app and the tablet app talk. The same messages are used on both transports:

- **USB accessory** (preferred). The Mac switches the tablet into Android Open Accessory mode
  (manufacturer `blackb1rd`, model `SideScreen`) and exchanges bulk transfers with the app.
- **TCP through `adb reverse`** (fallback). The app connects to `127.0.0.1:27183` on the tablet,
  which adb forwards to the Mac. Needs USB debugging and tops out around 10–15 Mbit/s.

## Framing

Every message, in both directions:

| Bytes | Field |
|---|---|
| 1 | marker `0x5A` |
| 1 | type |
| 4 | payload length, big-endian |
| n | payload |

All integers are big-endian; floats are IEEE-754 `float32`, big-endian.

A reader accepts a header only if the marker matches **and** the length is plausible for that type
(see the tables below). Otherwise it drops one byte and tries again. That way it resynchronises
after a reconnect, when stale bytes from an earlier session can still be in the USB pipe. A reader
that had to skip bytes asks for a fresh start (the tablet re-sends HELLO).

Over USB, a transfer whose size is a multiple of the endpoint's packet size gets a trailing NOP
message instead of a zero-length packet.

## Mac → tablet

| Type | Name | Length | Payload |
|---|---|---|---|
| 0 | NOP | 0 | heartbeat every 0.5 s on USB, and padding |
| 1 | SIZE | 8 or 12 | width u32, height u32, codec u32 (0 = H.264, 1 = HEVC); (re)create the decoder |
| 2 | CONFIG | 1–4096 | codec parameter sets (VPS/SPS/PPS) in Annex-B format |
| 3 | FRAME | ≥ 6 | flags u8 (bit 0 = keyframe), frame id u32, one Annex-B access unit |
| 4 | DISPLAY | 1 | 1 = Mac display awake, 0 = asleep (the tablet may let its screen sleep) |
| 5 | HELLO_REQUEST | 0 | please send HELLO (sent until HELLO arrives, e.g. after the link reopens) |

## Tablet → Mac

| Type | Name | Length | Payload |
|---|---|---|---|
| 0 | NOP | 0 | heartbeat every 0.5 s; on USB, silence for 3 s means the app is gone |
| 10 | TOUCH | 9 | action u8 (0 down, 1 move, 2 up, 3 right-click), x f32, y f32 |
| 11 | HELLO | 12–64 | screen width u32, height u32, dpi u32, capabilities u32 (bit 0 = hardware HEVC), max decode width u32, max decode height u32 |
| 12 | ACK | 4 | id of the frame just decoded (implies all earlier frames) |
| 13 | SCROLL | 19 | kind u8 (0 fingers, 1 momentum), phase u8 (1 began, 2 changed, 4 ended), last u8 (1 = gesture over), x f32, y f32, dx f32, dy f32 |
| 14 | ZOOM | 9 | direction i8 (+1 in, −1 out), x f32, y f32 |

Positions (`x`, `y`) and scroll deltas are fractions of the tablet's video view (0–1). Scroll deltas
are positive when the fingers move right/down.

## Session

1. The tablet connects and sends **HELLO** with its screen size, pixel density, decoder
   capabilities and the largest size its hardware decoder handles at 60 fps.
2. The Mac creates a virtual display to match. It encodes at a size that fits the decoder limit,
   in HEVC when the tablet can decode it in hardware.
3. The Mac sends **SIZE**, then **CONFIG** and a keyframe **FRAME**; after that, frames as the
   screen changes.
4. The tablet **ACK**s every decoded frame. The Mac keeps at most 3 frames unacknowledged, skipping
   captures while the tablet is behind. It stops waiting after 250 ms without an ACK.
5. A repeated **HELLO** (for example after the tablet resynchronised) makes the Mac send SIZE and a
   keyframe again.
