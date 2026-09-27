//! The SideScreen wire protocol, shared with the Mac app and the Android app. See PROTOCOL.md
//! in https://github.com/blackb1rd/sidescreen for the full description.
//!
//! Every message, in both directions: `[0x5A][type u8][length u32 BE][payload]`. A reader only
//! accepts a header whose marker matches and whose length is plausible for its type, so it can
//! resynchronise after a reconnect (stale bytes can still be in the USB pipe).

pub const MARKER: u8 = 0x5A;
pub const HEADER: usize = 6;

/// Message types. Host -> tablet: 0-9 and 20-22; tablet -> host: 0, 10-19 and 23.
pub mod msg {
    pub const NOP: u8 = 0;
    pub const SIZE: u8 = 1;
    pub const CONFIG: u8 = 2;
    pub const FRAME: u8 = 3;
    pub const DISPLAY: u8 = 4;
    pub const HELLO_REQUEST: u8 = 5;
    pub const PAIR: u8 = 7;
    pub const TOUCH: u8 = 10;
    pub const HELLO: u8 = 11;
    pub const ACK: u8 = 12;
    pub const SCROLL: u8 = 13;
    pub const ZOOM: u8 = 14;
    pub const PEN: u8 = 15;
    pub const SHARE_SIZE: u8 = 16;
    pub const SHARE_CONFIG: u8 = 17;
    pub const SHARE_FRAME: u8 = 18;
    pub const VIEWING: u8 = 19;
    pub const SHARE_STATUS: u8 = 23;
    /// Tablet ID: an idle Wi-Fi connection the tablet keeps ready while it is on USB.
    pub const STANDBY: u8 = 28;
}

pub fn encode(kind: u8, payload: &[u8]) -> Vec<u8> {
    let mut d = Vec::with_capacity(HEADER + payload.len());
    d.push(MARKER);
    d.push(kind);
    d.extend_from_slice(&(payload.len() as u32).to_be_bytes());
    d.extend_from_slice(payload);
    d
}

/// Header sanity for messages the host receives, so stale bytes rarely pass for one.
pub fn plausible(kind: u8, len: usize) -> bool {
    match kind {
        msg::NOP => len == 0,
        msg::TOUCH | msg::ZOOM => len == 9,
        msg::HELLO => (12..=64).contains(&len),
        msg::ACK => len == 4,
        msg::SCROLL => len == 19,
        msg::PEN => len == 14,
        msg::VIEWING => len == 1,
        msg::SHARE_SIZE => len == 12,
        msg::SHARE_CONFIG => (1..=4096).contains(&len),
        msg::SHARE_FRAME => (2..=16 << 20).contains(&len),
        msg::SHARE_STATUS => len == 2,
        msg::STANDBY => len == 16,
        _ => false,
    }
}

pub fn u32_at(p: &[u8], i: usize) -> u32 {
    u32::from_be_bytes([p[i], p[i + 1], p[i + 2], p[i + 3]])
}

pub fn f32_at(p: &[u8], i: usize) -> f32 {
    f32::from_bits(u32_at(p, i))
}

/// Collects bytes from a link and yields complete messages, skipping anything that doesn't
/// start a plausible one.
#[derive(Default)]
pub struct Reader {
    buf: Vec<u8>,
    start: usize,
    /// Bytes skipped to get back in sync since the last check.
    pub skipped: usize,
}

impl Reader {
    pub fn push(&mut self, data: &[u8]) {
        if self.start > 0 && self.start * 2 > self.buf.len() {
            self.buf.drain(..self.start);
            self.start = 0;
        }
        self.buf.extend_from_slice(data);
    }

    pub fn next(&mut self) -> Option<(u8, Vec<u8>)> {
        loop {
            let b = &self.buf[self.start..];
            if b.len() < HEADER {
                return None;
            }
            let len = u32_at(b, 2) as usize;
            if b[0] != MARKER || !plausible(b[1], len) {
                self.start += 1;
                self.skipped += 1;
                continue;
            }
            if b.len() < HEADER + len {
                return None;
            }
            let kind = b[1];
            let payload = b[HEADER..HEADER + len].to_vec();
            self.start += HEADER + len;
            return Some((kind, payload));
        }
    }
}

/// Splits an Annex-B stream into NAL units (without start codes).
pub fn nal_units(annex_b: &[u8]) -> Vec<&[u8]> {
    let mut starts = Vec::new(); // (start code offset, payload offset)
    let mut i = 0;
    while i + 3 <= annex_b.len() {
        if annex_b[i] == 0 && annex_b[i + 1] == 0 && annex_b[i + 2] == 1 {
            let sc = if i > 0 && annex_b[i - 1] == 0 {
                i - 1
            } else {
                i
            };
            starts.push((sc, i + 3));
            i += 3;
        } else {
            i += 1;
        }
    }
    starts
        .iter()
        .enumerate()
        .map(|(n, &(_, from))| {
            let end = starts.get(n + 1).map_or(annex_b.len(), |s| s.0);
            &annex_b[from..end]
        })
        .collect()
}

/// H.264 NAL unit type.
pub fn nal_type(nal: &[u8]) -> u8 {
    nal.first().map_or(0, |b| b & 0x1f)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn ack(id: u32) -> Vec<u8> {
        encode(msg::ACK, &id.to_be_bytes())
    }

    #[test]
    fn encodes_marker_type_and_length() {
        assert_eq!(encode(msg::DISPLAY, &[1]), vec![0x5A, 4, 0, 0, 0, 1, 1]);
    }

    #[test]
    fn reads_consecutive_and_split_messages() {
        let mut r = Reader::default();
        let whole = [ack(7), ack(8)].concat();
        r.push(&whole[..13]); // the first message plus part of the second
        assert_eq!(r.next(), Some((msg::ACK, 7u32.to_be_bytes().to_vec())));
        assert_eq!(r.next(), None);
        r.push(&whole[13..]);
        assert_eq!(r.next(), Some((msg::ACK, 8u32.to_be_bytes().to_vec())));
    }

    #[test]
    fn resynchronises_past_garbage() {
        let mut r = Reader::default();
        r.push(&[0x01, 0x5A, 0x0C, 0xFF, 0xFF, 0xFF, 0xFF, 0x33]);
        r.push(&ack(5));
        assert_eq!(r.next(), Some((msg::ACK, 5u32.to_be_bytes().to_vec())));
        assert_eq!(r.skipped, 8);
    }

    #[test]
    fn rejects_implausible_headers() {
        assert!(plausible(msg::ACK, 4));
        assert!(!plausible(msg::ACK, 5));
        assert!(!plausible(msg::FRAME, 100)); // host -> tablet only
        assert!(plausible(msg::PEN, 14));
    }

    #[test]
    fn splits_annex_b() {
        let s = [
            0, 0, 0, 1, 0x67, 1, 2, 0, 0, 1, 0x68, 3, 0, 0, 0, 1, 0x65, 4, 5,
        ];
        let nals = nal_units(&s);
        assert_eq!(
            nals,
            vec![&[0x67, 1, 2][..], &[0x68, 3][..], &[0x65, 4, 5][..]]
        );
        assert_eq!(nal_type(nals[0]), 7);
    }
}
