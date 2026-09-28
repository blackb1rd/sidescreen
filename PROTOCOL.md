# Spanly protocol

How the Mac app and the tablet app talk. The same messages are used on both transports:

- **USB accessory** (preferred). The Mac switches the tablet into Android Open Accessory mode
  (manufacturer `caigenix`, model `Spanly`) and exchanges bulk transfers with the app.
- **Wi-Fi** (optional). The Mac advertises `_spanly._tcp` over Bonjour on port 27184 when
  "Allow Wi-Fi Connection" is on. Traffic is encrypted, and only tablets paired over USB can
  connect (see *Wi-Fi* below).
- **TCP through `adb reverse`** (fallback). The app connects to `127.0.0.1:27183` on the tablet,
  which adb forwards to the Mac. Needs USB debugging and tops out around 10–15 Mbit/s.

The tablet tries them in that order (USB, adb, Wi-Fi) and moves to USB whenever it appears.

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
| 0 | NOP | 0 | heartbeat every 0.5 s (USB and Wi-Fi), and padding |
| 1 | SIZE | 8 or 12 | width u32, height u32, codec u32 (0 = H.264, 1 = HEVC); (re)create the decoder |
| 2 | CONFIG | 1–4096 | codec parameter sets (VPS/SPS/PPS) in Annex-B format |
| 3 | FRAME | ≥ 6 | flags u8 (bit 0 = keyframe), frame id u32, one Annex-B access unit |
| 4 | DISPLAY | 1 | 1 = Mac display awake, 0 = asleep (the tablet may let its screen sleep) |
| 5 | HELLO_REQUEST | 0 | please send HELLO (sent until HELLO arrives, e.g. after the link reopens) |
| 6 | AUDIO | 4–65536 | 48 kHz 16-bit little-endian interleaved stereo PCM (when sound is on) |
| 7 | PAIR | 33–288 | Wi-Fi secret (32 bytes) + the Mac's name (UTF-8); **sent only over USB** |
| 8 | SHARE_START | 0 | please show your screen on the Mac (the user must agree on the tablet) |
| 9 | SHARE_STOP | 0 | stop showing your screen |
| 29 | UDP_OFFER | 18 | Wi-Fi only: token (16 bytes) + UDP port u16; register there to get the video as datagrams |
| 30 | UDP_READY | 0 | from now on FRAME and CONFIG come over UDP |
| 32 | STREAM_STOP | 0 | the user stopped sharing on the computer: drop the picture and show a "stopped" screen until the next SIZE |
| 26 | MIC_START | 0 | send your microphone (MIC_AUDIO) — the Mac plays it into its "Spanly Microphone" device |
| 27 | MIC_STOP | 0 | stop sending your microphone |
| 20 | REMOTE_POINTER | 9 | action u8 (0 down, 1 move, 2 up, 3 long press), x f32, y f32 |
| 21 | REMOTE_SCROLL | 16 | x, y, dx, dy (f32, fractions of the tablet's screen) |
| 22 | REMOTE_KEY | 2–258 | kind u8 + data: 0 UTF-8 text, 1 Android key code u16, 2 global action u8 (1 back, 2 home, 3 recents) |

## Tablet → Mac

| Type | Name | Length | Payload |
|---|---|---|---|
| 0 | NOP | 0 | heartbeat every 0.5 s; on USB, silence for 3 s means the app is gone |
| 10 | TOUCH | 9 | action u8 (0 down, 1 move, 2 up, 3 right-click), x f32, y f32 |
| 11 | HELLO | 12–64 | screen width u32, height u32, dpi u32, capabilities u32 (bit 0 = hardware HEVC, bit 1 = can take video over UDP), max decode width u32, max decode height u32, tablet ID (16 random bytes, kept by the app), tablet name (UTF-8, ≤ 24 bytes) |
| 12 | ACK | 4 | id of the frame just decoded (implies all earlier frames) |
| 13 | SCROLL | 19 | kind u8 (0 fingers, 1 momentum), phase u8 (1 began, 2 changed, 4 ended), last u8 (1 = gesture over), x f32, y f32, dx f32, dy f32 |
| 14 | ZOOM | 9 | direction i8 (+1 in, −1 out), x f32, y f32 |
| 15 | PEN | 14 | action u8 (0 hover, 1 down, 2 move, 3 up), buttons u8 (bit 0 = barrel button), x f32, y f32, pressure f32 |
| 16 | SHARE_SIZE | 12 | the tablet's shared screen: width u32, height u32, codec u32 (0 = H.264) |
| 17 | SHARE_CONFIG | 1–4096 | its SPS/PPS, Annex-B |
| 18 | SHARE_FRAME | ≥ 2 | flags u8 (bit 0 = keyframe) + one Annex-B access unit |
| 19 | VIEWING | 1 | 1 = the app shows the Mac's screen, 0 = it's in the background (the Mac stops encoding) |
| 24 | SHARE_AUDIO | 4–65536 | the tablet's own sound while shared: 48 kHz 16-bit little-endian interleaved stereo PCM |
| 25 | MIC_AUDIO | 2–65536 | the tablet's microphone: 48 kHz 16-bit little-endian mono PCM |
| 31 | KEYFRAME_REQUEST | 0 | a video frame sent over UDP was lost: send a keyframe |
| 28 | STANDBY | 16 | tablet ID: this Wi-Fi connection is kept idle while the tablet is on USB (see below) |
| 23 | SHARE_STATUS | 2 | state u8 (0 stopped, 1 sharing, 2 declined), control u8 (1 = the tablet's control service is on) |

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

## Several tablets

The tablet ID in HELLO tells the Mac which tablet a connection belongs to. Each tablet gets its
own virtual display, capture and encoder; a tablet that moves from USB to Wi-Fi keeps its display.
The Mac's sound, the microphone and "Show Tablet Screen" go to the first tablet connected. A
HELLO without an ID (older apps) counts as one shared tablet.

## Rotation and mirroring

The tablet sends a new **HELLO** when it turns. The Mac reshapes the same virtual display
(portrait or landscape), so its windows stay put, and then sends SIZE and a keyframe. In mirror
mode, the Mac streams its main display instead of a virtual one.

## Wi-Fi

1. While the tablet is connected over USB, the Mac sends **PAIR** with a random 32-byte secret.
   The tablet stores it. Plugging in is the trust step; "Forget Paired Tablets" on the Mac makes
   a new secret.
2. The tablet finds the Mac's Bonjour service and connects over TCP. It sends `"SPW1"` and a random
   16-byte nonce, and the Mac answers with its own 16-byte nonce.
3. Both derive two keys with HKDF-SHA256 (input: the secret; salt: client nonce ‖ server nonce;
   info `"spanly c2s"` and `"spanly s2c"`, 32 bytes each).
4. Everything else is records: length u32 + AES-256-GCM(ciphertext ‖ 16-byte tag). The nonce is
   4 zero bytes followed by a big-endian u64 counter per direction. Inside the records are the
   normal messages above.

While on USB (or adb), the tablet keeps a second connection open over Wi-Fi: it sends
**STANDBY** instead of HELLO, and after that only answers the Mac's heartbeats (a NOP for each
NOP). When the cable comes out it sends HELLO on that connection and the stream carries on over
Wi-Fi straight away. The Mac streams on whichever connection the tablet last sent HELLO on.

A peer without the secret fails its first record and is dropped.

Finding the Mac: its Bonjour service, and a UDP **beacon** it sends every second to port 27184
while Wi-Fi is allowed: `"SPB1"` + its name (the name from PAIR), as an IPv4 broadcast on each
network and an IPv6 all-nodes multicast (`ff02::1`) on each interface. The beacon reaches the
tablet where Bonjour may not, e.g. when the Mac has joined the tablet's own hotspot or on
IPv6-only phone hotspots; the tablet connects to the address it came from. The Mac adapts the video bitrate
to the latency it sees in ACKs, and allows 6 frames in flight on Wi-Fi.

## Showing the tablet on the Mac

The Mac sends **SHARE_START**. The tablet asks the user (Android's screen-capture consent, entire
screen) and answers with **SHARE_STATUS**. It then sends **SHARE_SIZE**, **SHARE_CONFIG** and
**SHARE_FRAME**s from its hardware encoder. Mouse and keyboard input in the Mac's window goes back
as **REMOTE_*** messages. The tablet performs them through its accessibility service, which the
user turns on once.

## Video over UDP (Wi-Fi)

TCP holds back everything behind a lost packet, which shows as stutter on a busy Wi-Fi. When the
tablet sets capability bit 1 on a Wi-Fi link, the host sends **UDP_OFFER** with a random token and
its UDP port (27185). The tablet sends `"SPU1"` + token from its own UDP socket, every 250 ms until
**UDP_READY** and every 2 s after that; the host sends the video to where those came from. Only
FRAME and CONFIG move to UDP; everything else stays on the encrypted TCP connection.

Each datagram (at most ~1200 bytes) is `seq u64 BE` + AES-256-GCM(`type u8, id u32, index u16,
count u16, chunk`), nonce = seq, with the key HKDF-SHA256(secret, client nonce ‖ server nonce,
`"spanly udp s2c"`). The tablet reassembles each message from its chunks. When a picture is lost
it drops the following ones and sends **KEYFRAME_REQUEST** (at most every 300 ms) until a keyframe
arrives.
