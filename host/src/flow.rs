//! Frames sent but not yet decoded on the tablet (it ACKs each one), the latency those ACKs
//! reveal, and the adaptive bitrate built on them. Same rules as the Mac app.

use std::collections::BTreeMap;
use std::sync::Mutex;
use std::time::{Duration, Instant};

pub struct Flow {
    inner: Mutex<Inner>,
}

struct Inner {
    max_in_flight: u32,
    next_id: u32,
    last_acked: u32,
    last_ack: Instant,
    sent_at: BTreeMap<u32, Instant>,
    window_acks: u32,
    window_latency_ms: f64,
    window_skipped: u32,
}

pub struct Window {
    pub acks: u32,
    pub avg_latency_ms: f64,
    pub skipped: u32,
}

impl Default for Flow {
    fn default() -> Self {
        Self {
            inner: Mutex::new(Inner {
                max_in_flight: 3,
                next_id: 1,
                last_acked: 0,
                last_ack: Instant::now(),
                sent_at: BTreeMap::new(),
                window_acks: 0,
                window_latency_ms: 0.0,
                window_skipped: 0,
            }),
        }
    }
}

impl Flow {
    /// Forget what's in flight (new session). Wi-Fi's longer round trip needs more frames in flight.
    pub fn reset(&self, max_in_flight: u32) {
        let mut s = self.inner.lock().unwrap();
        s.last_acked = s.next_id.wrapping_sub(1);
        s.last_ack = Instant::now();
        s.sent_at.clear();
        s.max_in_flight = max_in_flight;
    }

    /// Id for a frame about to be sent; `captured` is when its picture was taken.
    pub fn register(&self, captured: Instant) -> u32 {
        let mut s = self.inner.lock().unwrap();
        let id = s.next_id;
        s.next_id = s.next_id.wrapping_add(1);
        s.sent_at.insert(id, captured);
        id
    }

    /// The tablet decoded frame `id` (and so everything before it). Returns its latency.
    pub fn acked(&self, id: u32) -> Option<Duration> {
        let mut s = self.inner.lock().unwrap();
        if id <= s.last_acked {
            return None;
        }
        s.last_acked = id;
        s.last_ack = Instant::now();
        let captured = s.sent_at.get(&id).copied();
        s.sent_at.retain(|&k, _| k > id);
        let latency = captured.map(|t| t.elapsed())?;
        s.window_acks += 1;
        s.window_latency_ms += latency.as_secs_f64() * 1000.0;
        Some(latency)
    }

    /// Skip capturing while the tablet is behind; give up waiting after 250 ms without ACKs.
    pub fn may_send(&self) -> bool {
        let mut s = self.inner.lock().unwrap();
        if s.next_id.wrapping_sub(1).wrapping_sub(s.last_acked) < s.max_in_flight {
            return true;
        }
        if s.last_ack.elapsed() > Duration::from_millis(250) {
            s.last_acked = s.next_id.wrapping_sub(1);
            s.last_ack = Instant::now();
            s.sent_at.clear();
            return true;
        }
        s.window_skipped += 1;
        false
    }

    pub fn take_window(&self) -> Window {
        let mut s = self.inner.lock().unwrap();
        let w = Window {
            acks: s.window_acks,
            avg_latency_ms: if s.window_acks > 0 {
                s.window_latency_ms / s.window_acks as f64
            } else {
                0.0
            },
            skipped: s.window_skipped,
        };
        s.window_acks = 0;
        s.window_latency_ms = 0.0;
        s.window_skipped = 0;
        w
    }
}

/// Every second: lower the bitrate when frames queue up or latency climbs, and creep back up
/// when the link has headroom. Returns the new bitrate if it changed meaningfully.
pub fn adapt_bitrate(current: f64, max: f64, w: &Window) -> Option<f64> {
    if w.acks + w.skipped < 10 {
        return None; // screen idle: nothing to learn
    }
    let target = if w.skipped > (w.acks + w.skipped) / 5 || w.avg_latency_ms > 90.0 {
        (current * 0.75).max(3.0)
    } else if w.skipped == 0 && w.avg_latency_ms < 60.0 {
        (current * 1.1).min(max)
    } else {
        current
    };
    ((target - current).abs() >= 0.5).then_some(target)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn limits_frames_in_flight_until_acked() {
        let f = Flow::default();
        f.reset(3);
        for _ in 0..3 {
            assert!(f.may_send());
            f.register(Instant::now());
        }
        assert!(!f.may_send());
        assert!(f.acked(2).is_some());
        assert!(f.may_send());
        assert!(f.acked(1).is_none()); // older than the last ACK
    }

    #[test]
    fn lowers_the_bitrate_when_frames_back_up() {
        let bad = Window {
            acks: 20,
            avg_latency_ms: 120.0,
            skipped: 10,
        };
        assert_eq!(adapt_bitrate(20.0, 20.0, &bad), Some(15.0));
        let good = Window {
            acks: 60,
            avg_latency_ms: 30.0,
            skipped: 0,
        };
        assert_eq!(adapt_bitrate(10.0, 20.0, &good), Some(11.0));
        assert_eq!(adapt_bitrate(20.0, 20.0, &good), None); // already at the ceiling
    }
}
