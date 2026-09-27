//! Windows: DXGI Desktop Duplication captures a monitor (the Virtual Display Driver's virtual
//! monitor in extend mode, the primary one in mirror mode), Media Foundation encodes H.264, and
//! SendInput turns touches into mouse input.

mod capture;
mod encoder;
mod input;

use crate::backend::{self, Backend, EncodedFrame, Gate, Mode, Pointer, StreamRequest};
use crossbeam_channel::Sender;
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, AtomicU32, Ordering};
use std::time::{Duration, Instant};
use windows::Win32::System::Com::{COINIT_MULTITHREADED, CoInitializeEx};

#[derive(Default)]
pub struct WindowsBackend {
    stop: Option<Arc<AtomicBool>>,
    keyframe: Arc<AtomicBool>,
    bitrate: Arc<AtomicU32>,
    input: Option<input::Input>,
}

impl WindowsBackend {
    pub fn new() -> Result<Self, String> {
        Ok(Self::default())
    }
}

impl Backend for WindowsBackend {
    fn start(
        &mut self,
        req: &StreamRequest,
        gate: Gate,
        out: Sender<EncodedFrame>,
    ) -> Result<(u32, u32), String> {
        self.stop();
        let stop = Arc::new(AtomicBool::new(false));
        self.stop = Some(stop.clone());
        self.bitrate.store(req.bitrate, Ordering::Relaxed);
        let (keyframe, bitrate) = (self.keyframe.clone(), self.bitrate.clone());
        let (mode, tablet, limit, fps) = (
            req.mode,
            (req.tablet_width, req.tablet_height),
            (req.max_width, req.max_height),
            req.fps,
        );
        let (ready_tx, ready_rx) = std::sync::mpsc::channel();

        // COM objects stay on this one thread.
        std::thread::spawn(move || {
            unsafe {
                let _ = CoInitializeEx(None, COINIT_MULTITHREADED);
            }
            let setup = || -> Result<_, String> {
                let mut o = capture::pick(mode)?;
                if mode == Mode::Extend {
                    capture::set_resolution(&o, tablet.0, tablet.1);
                    // Pick it up again with its new size.
                    o = capture::pick(mode)?;
                }
                let (mw, mh) = (
                    (o.rect.right - o.rect.left) as u32,
                    (o.rect.bottom - o.rect.top) as u32,
                );
                let (lw, lh) = if limit.0 > 0 { limit } else { tablet };
                let (w, h) = match mode {
                    Mode::Extend => backend::fit(mw, mh, lw, lh),
                    Mode::Mirror => backend::fit(mw, mh, lw.max(lh), lw.max(lh)),
                };
                let enc = encoder::H264Encoder::new(w, h, fps, bitrate.load(Ordering::Relaxed))
                    .map_err(|e| format!("H.264 encoder: {e}"))?;
                let dup =
                    capture::Duplicator::new(&o).map_err(|e| format!("screen capture: {e}"))?;
                Ok((o, enc, dup, w, h))
            };
            let (o, mut enc, mut dup, w, h) = match setup() {
                Ok(s) => s,
                Err(e) => {
                    let _ = ready_tx.send(Err(e));
                    return;
                }
            };
            let _ = ready_tx.send(Ok((w, h, o.rect)));
            let mut nv12 = Vec::new();
            let mut have_picture = false;
            let mut current_bitrate = bitrate.load(Ordering::Relaxed);
            while !stop.load(Ordering::Relaxed) {
                let b = bitrate.load(Ordering::Relaxed);
                if b != current_bitrate {
                    enc.set_bitrate(b);
                    current_bitrate = b;
                }
                let mut captured = Instant::now();
                let fresh = match dup.next(100, |bgra, sw, sh, pitch| {
                    capture::bgra_to_nv12(bgra, sw, sh, pitch, w, h, &mut nv12)
                }) {
                    Ok(f) => f,
                    Err(e) => {
                        log::warn!("screen capture interrupted ({e}); restarting it");
                        std::thread::sleep(Duration::from_millis(200));
                        match capture::Duplicator::new(&o) {
                            Ok(d) => dup = d,
                            Err(e) => log::warn!("screen capture: {e}"),
                        }
                        continue;
                    }
                };
                have_picture |= fresh;
                // A still screen produces no frames: re-encode the last picture for a requested keyframe.
                let want_key = keyframe.swap(false, Ordering::Relaxed);
                if !(fresh || (want_key && have_picture)) || !gate() {
                    if want_key {
                        keyframe.store(true, Ordering::Relaxed);
                    }
                    continue;
                }
                if want_key {
                    enc.force_keyframe();
                }
                if !fresh {
                    captured = Instant::now();
                }
                match enc.encode(&nv12) {
                    Ok(units) => {
                        for (data, key) in units {
                            if out
                                .send(EncodedFrame {
                                    data,
                                    key,
                                    captured,
                                })
                                .is_err()
                            {
                                return;
                            }
                        }
                    }
                    Err(e) => log::warn!("H.264 encoder: {e}"),
                }
            }
        });

        let (w, h, rect) = ready_rx
            .recv()
            .map_err(|_| "capture thread ended".to_string())??;
        self.input = Some(input::Input::new(rect));
        Ok((w, h))
    }

    fn stop(&mut self) {
        if let Some(s) = self.stop.take() {
            s.store(true, Ordering::Relaxed);
        }
        self.input = None;
    }

    fn request_keyframe(&mut self) {
        self.keyframe.store(true, Ordering::Relaxed);
    }

    fn set_bitrate(&mut self, bps: u32) {
        self.bitrate.store(bps, Ordering::Relaxed);
    }

    fn pointer(&mut self, action: Pointer, x: f32, y: f32) {
        if let Some(i) = &mut self.input {
            i.pointer(action, x, y);
        }
    }

    fn scroll(&mut self, x: f32, y: f32, dx: f32, dy: f32) {
        if let Some(i) = &mut self.input {
            i.scroll(x, y, dx, dy);
        }
    }

    fn zoom(&mut self, direction: i8) {
        if let Some(i) = &mut self.input {
            i.zoom(direction);
        }
    }
}
