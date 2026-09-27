//! What each OS provides: a display to stream (captured and H.264-encoded) and input
//! injection. Linux: xdg-desktop-portal + PipeWire + GStreamer. Windows: DXGI Desktop
//! Duplication + Media Foundation + SendInput. `test-pattern`: a synthetic source.

use crossbeam_channel::Sender;
use std::sync::Arc;
use std::time::Instant;

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Mode {
    /// A separate second screen (a virtual monitor).
    Extend,
    /// The same picture as the main screen.
    Mirror,
}

pub struct StreamRequest {
    pub mode: Mode,
    /// The tablet's screen as held (landscape or portrait), in pixels.
    pub tablet_width: u32,
    pub tablet_height: u32,
    /// Largest size to encode (the tablet's hardware decoder limit), same aspect as the tablet.
    pub max_width: u32,
    pub max_height: u32,
    pub fps: u32,
    pub bitrate: u32,
}

/// One H.264 access unit in Annex-B form.
pub struct EncodedFrame {
    pub data: Vec<u8>,
    pub key: bool,
    /// When its picture was captured (for latency).
    pub captured: Instant,
}

/// Asked before encoding each captured picture: false means skip it (the tablet is behind or
/// not looking). Skipping before the encoder keeps the reference chain intact.
pub type Gate = Arc<dyn Fn() -> bool + Send + Sync>;

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Pointer {
    Down,
    Move,
    Up,
    RightClick,
    /// Pen hovering above the screen: move without pressing.
    Hover,
}

pub trait Backend: Send {
    /// Start streaming; returns the encoded size.
    fn start(
        &mut self,
        req: &StreamRequest,
        gate: Gate,
        out: Sender<EncodedFrame>,
    ) -> Result<(u32, u32), String>;
    fn stop(&mut self);
    fn request_keyframe(&mut self);
    fn set_bitrate(&mut self, bps: u32);
    /// Positions are fractions (0-1) of the streamed display.
    fn pointer(&mut self, action: Pointer, x: f32, y: f32);
    /// Deltas are fractions of the display; content follows the fingers.
    fn scroll(&mut self, x: f32, y: f32, dx: f32, dy: f32);
    /// Pinch steps: +1 zoom in, -1 zoom out (Ctrl + / Ctrl -).
    fn zoom(&mut self, direction: i8);
}

pub fn create(test_pattern: bool) -> Result<Box<dyn Backend>, String> {
    #[cfg(feature = "test-pattern")]
    if test_pattern {
        return Ok(Box::new(
            crate::platform::test_pattern::TestPattern::default(),
        ));
    }
    let _ = test_pattern;
    #[cfg(target_os = "linux")]
    return crate::platform::linux::LinuxBackend::new().map(|b| Box::new(b) as Box<dyn Backend>);
    #[cfg(windows)]
    return crate::platform::windows::WindowsBackend::new()
        .map(|b| Box::new(b) as Box<dyn Backend>);
    #[allow(unreachable_code)]
    Err("no screen capture backend for this OS (build with --features test-pattern to try the link)".into())
}

/// Fit `(w, h)` inside `(max_w, max_h)` keeping its shape, in whole 16-pixel blocks.
pub fn fit(w: u32, h: u32, max_w: u32, max_h: u32) -> (u32, u32) {
    let scale = if max_w > 0 && max_h > 0 {
        (max_w as f64 / w as f64)
            .min(max_h as f64 / h as f64)
            .min(1.0)
    } else {
        1.0
    };
    (
        ((w as f64 * scale) as u32) & !15,
        ((h as f64 * scale) as u32) & !15,
    )
}

#[cfg(test)]
mod tests {
    #[test]
    fn fits_the_decoder_limit() {
        assert_eq!(super::fit(2560, 1600, 2304, 1440), (2304, 1440));
        assert_eq!(super::fit(1600, 2560, 1440, 2304), (1440, 2304));
        assert_eq!(super::fit(1280, 800, 2560, 1440), (1280, 800));
    }
}
