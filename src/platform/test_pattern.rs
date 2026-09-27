//! A moving test pattern encoded in software (openh264): exercises the links and the protocol
//! with a real tablet on any OS, without a screen-capture backend. Input is only logged.

use crate::backend::{self, Backend, EncodedFrame, Gate, Pointer, StreamRequest};
use crossbeam_channel::Sender;
use openh264::OpenH264API;
use openh264::encoder::{
    BitRate, Encoder, EncoderConfig, FrameRate, IntraFramePeriod, RateControlMode, UsageType,
};
use openh264::formats::YUVBuffer;
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};
use std::time::{Duration, Instant};

#[derive(Default)]
pub struct TestPattern {
    stop: Option<Arc<AtomicBool>>,
    keyframe: Arc<AtomicBool>,
}

impl Backend for TestPattern {
    fn start(
        &mut self,
        req: &StreamRequest,
        gate: Gate,
        out: Sender<EncodedFrame>,
    ) -> Result<(u32, u32), String> {
        // Software encoding: keep it light.
        let (w, h) = backend::fit(
            req.tablet_width,
            req.tablet_height,
            req.max_width.min(1280),
            req.max_height.min(1280),
        );
        log::info!("test pattern ({:?} mode is only simulated)", req.mode);
        let config = EncoderConfig::new()
            .bitrate(BitRate::from_bps(req.bitrate))
            .max_frame_rate(FrameRate::from_hz(req.fps as f32))
            .usage_type(UsageType::ScreenContentRealTime)
            .rate_control_mode(RateControlMode::Bitrate)
            .skip_frames(false)
            .intra_frame_period(IntraFramePeriod::from_num_frames(req.fps * 10));
        let mut enc = Encoder::with_api_config(OpenH264API::from_source(), config)
            .map_err(|e| e.to_string())?;
        let stop = Arc::new(AtomicBool::new(false));
        self.stop = Some(stop.clone());
        let keyframe = self.keyframe.clone();
        let interval = Duration::from_secs_f64(1.0 / req.fps as f64);
        std::thread::spawn(move || {
            let (wu, hu) = (w as usize, h as usize);
            let mut yuv = vec![128u8; wu * hu * 3 / 2];
            let mut n = 0usize;
            while !stop.load(Ordering::Relaxed) {
                let t = Instant::now();
                if gate() {
                    draw(&mut yuv[..wu * hu], wu, hu, n);
                    if keyframe.swap(false, Ordering::Relaxed) {
                        enc.force_intra_frame();
                    }
                    let Ok(bits) = enc.encode(&YUVBuffer::from_vec(yuv.clone(), wu, hu)) else {
                        break;
                    };
                    let data = bits.to_vec();
                    let key = crate::protocol::nal_units(&data)
                        .iter()
                        .any(|nal| crate::protocol::nal_type(nal) == 5);
                    if !data.is_empty()
                        && out
                            .send(EncodedFrame {
                                data,
                                key,
                                captured: t,
                            })
                            .is_err()
                    {
                        break;
                    }
                    n += 1;
                }
                std::thread::sleep(interval.saturating_sub(t.elapsed()));
            }
        });
        Ok((w, h))
    }

    fn stop(&mut self) {
        if let Some(s) = self.stop.take() {
            s.store(true, Ordering::Relaxed);
        }
    }

    fn request_keyframe(&mut self) {
        self.keyframe.store(true, Ordering::Relaxed);
    }

    fn set_bitrate(&mut self, bps: u32) {
        log::debug!("test pattern: bitrate {bps} (fixed in this backend)");
    }

    fn pointer(&mut self, action: Pointer, x: f32, y: f32) {
        log::info!("test pattern: {action:?} at {x:.3}, {y:.3}");
    }

    fn scroll(&mut self, x: f32, y: f32, dx: f32, dy: f32) {
        log::info!("test pattern: scroll {dx:.4}, {dy:.4} at {x:.3}, {y:.3}");
    }

    fn zoom(&mut self, direction: i8) {
        log::info!("test pattern: zoom {direction}");
    }
}

/// A diagonal gradient with a bright bar sweeping across (luma only; chroma stays grey).
fn draw(y: &mut [u8], w: usize, h: usize, n: usize) {
    let bar = (n * 8) % w;
    for row in 0..h {
        for col in 0..w {
            y[row * w + col] = if col.abs_diff(bar) < 24 {
                235
            } else {
                ((row + col + n) / 8 % 160 + 30) as u8
            };
        }
    }
}
