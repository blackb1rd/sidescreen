//! Linux: the desktop portal shares the screen (a new virtual monitor, or the main one) and
//! injects input; GStreamer turns the PipeWire stream into H.264, preferring hardware encoders.

mod portal;

use crate::backend::{self, Backend, EncodedFrame, Gate, Mode, Pointer, StreamRequest};
use crossbeam_channel::Sender;
use gstreamer as gst;
use gstreamer::prelude::*;
use gstreamer_app as gst_app;
use gstreamer_video as gst_video;
use std::collections::VecDeque;
use std::os::fd::AsRawFd;
use std::sync::{Arc, Mutex};
use std::time::Instant;

/// When each picture entering the encoder was captured, by buffer timestamp.
type Captured = Arc<Mutex<VecDeque<(Option<gst::ClockTime>, Instant)>>>;

/// H.264 encoders in order of preference, and whether their bitrate is in kbit/s.
const ENCODERS: [(&str, bool); 6] = [
    ("vah264lpenc", true),  // VA-API low-power (Intel, AMD)
    ("vah264enc", true),    // VA-API
    ("vaapih264enc", true), // older VA-API plugin
    ("nvh264enc", true),    // NVIDIA NVENC
    ("x264enc", true),      // software
    ("openh264enc", false), // software, bit/s
];

pub struct LinuxBackend {
    rt: tokio::runtime::Runtime,
    cast: Option<portal::Cast>,
    pipeline: Option<gst::Pipeline>,
    encoder: Option<(gst::Element, bool)>,
}

impl LinuxBackend {
    pub fn new() -> Result<Self, String> {
        gst::init().map_err(|e| format!("GStreamer: {e}"))?;
        let rt = tokio::runtime::Builder::new_multi_thread()
            .worker_threads(1)
            .enable_all()
            .build()
            .map_err(|e| e.to_string())?;
        Ok(Self {
            rt,
            cast: None,
            pipeline: None,
            encoder: None,
        })
    }

    fn make(name: &str) -> Result<gst::Element, String> {
        gst::ElementFactory::make(name).build().map_err(|_| {
            format!(
                "GStreamer element '{name}' is missing (install the gstreamer plugins packages)"
            )
        })
    }

    fn encoder(bitrate: u32, fps: u32) -> Result<(gst::Element, bool), String> {
        let (name, kbps) = ENCODERS
            .iter()
            .find(|(n, _)| gst::ElementFactory::find(n).is_some())
            .ok_or(
                "no H.264 encoder found: install gstreamer1.0-plugins-bad (VA-API) or -ugly (x264)",
            )?;
        let enc = Self::make(name)?;
        let set = |k: &str, v: &str| {
            if enc.has_property(k) {
                enc.set_property_from_str(k, v);
            }
        };
        // Low latency: no B-frames, constant bitrate, a keyframe only every 10 s (plus on request).
        set(
            "bitrate",
            &if *kbps { bitrate / 1000 } else { bitrate }.to_string(),
        );
        set("rate-control", "cbr");
        set("b-frames", "0");
        set("bframes", "0");
        set("max-bframes", "0");
        set("key-int-max", &(fps * 10).to_string());
        set("gop-size", &(fps * 10).to_string());
        set("tune", "zerolatency");
        set("speed-preset", "ultrafast");
        set("zerolatency", "true");
        set("preset", "low-latency-hp");
        set("target-usage", "7");
        log::info!("H.264 encoder: {name}");
        Ok((enc, *kbps))
    }
}

impl Backend for LinuxBackend {
    fn start(
        &mut self,
        req: &StreamRequest,
        gate: Gate,
        out: Sender<EncodedFrame>,
    ) -> Result<(u32, u32), String> {
        self.stop();
        let cast = portal::start(&self.rt, req.mode)?;
        let (w, h) = match req.mode {
            Mode::Extend => backend::fit(
                req.tablet_width,
                req.tablet_height,
                req.max_width,
                req.max_height,
            ),
            Mode::Mirror => {
                // The main screen's shape, fitted into what the tablet can decode.
                let (mw, mh) = (cast.size.0.max(16) as u32, cast.size.1.max(16) as u32);
                let (lw, lh) = if req.max_width > 0 {
                    (req.max_width, req.max_height)
                } else {
                    (req.tablet_width, req.tablet_height)
                };
                backend::fit(mw, mh, lw.max(lh), lw.max(lh))
            }
        };

        let pipeline = gst::Pipeline::new();
        let src = Self::make("pipewiresrc")?;
        src.set_property("fd", cast.fd.as_raw_fd());
        src.set_property("path", cast.node.to_string());
        src.set_property("do-timestamp", true);
        if src.has_property("keepalive-time") {
            src.set_property("keepalive-time", 1000i32);
        }
        // In extend mode these caps ask the compositor for a virtual monitor of the tablet's size.
        let want = gst::ElementFactory::make("capsfilter")
            .build()
            .map_err(|e| e.to_string())?;
        if req.mode == Mode::Extend {
            want.set_property(
                "caps",
                gst::Caps::builder("video/x-raw")
                    .field("width", w as i32)
                    .field("height", h as i32)
                    .build(),
            );
        }
        let convert = Self::make("videoconvert")?;
        let scale = Self::make("videoscale")?;
        let raw = gst::ElementFactory::make("capsfilter")
            .build()
            .map_err(|e| e.to_string())?;
        raw.set_property(
            "caps",
            gst::Caps::builder("video/x-raw")
                .field("format", "NV12")
                .field("width", w as i32)
                .field("height", h as i32)
                .build(),
        );
        let (enc, kbps) = Self::encoder(req.bitrate, req.fps)?;
        let parse = Self::make("h264parse")?;
        parse.set_property_from_str("config-interval", "-1"); // SPS/PPS with every keyframe
        let annexb = gst::ElementFactory::make("capsfilter")
            .build()
            .map_err(|e| e.to_string())?;
        annexb.set_property(
            "caps",
            gst::Caps::builder("video/x-h264")
                .field("stream-format", "byte-stream")
                .field("alignment", "au")
                .build(),
        );
        let sink = gst_app::AppSink::builder()
            .sync(false)
            .max_buffers(4)
            .drop(true)
            .build();

        let elements = [
            &src,
            &want,
            &convert,
            &scale,
            &raw,
            &enc,
            &parse,
            &annexb,
            sink.upcast_ref(),
        ];
        pipeline.add_many(elements).map_err(|e| e.to_string())?;
        gst::Element::link_many(elements).map_err(|e| format!("GStreamer: {e}"))?;

        // Skip pictures before the encoder while the tablet is behind (keeps references intact),
        // and remember when each one was captured for latency stats.
        let captured: Captured = Arc::default();
        let c = captured.clone();
        enc.static_pad("sink")
            .ok_or("encoder has no sink pad")?
            .add_probe(gst::PadProbeType::BUFFER, move |_, info| {
                if !gate() {
                    return gst::PadProbeReturn::Drop;
                }
                let pts = info.buffer().and_then(|b| b.pts());
                let mut q = c.lock().unwrap();
                q.push_back((pts, Instant::now()));
                if q.len() > 64 {
                    q.pop_front();
                }
                gst::PadProbeReturn::Ok
            });
        sink.set_callbacks(
            gst_app::AppSinkCallbacks::builder()
                .new_sample(move |sink| {
                    let sample = sink.pull_sample().map_err(|_| gst::FlowError::Eos)?;
                    let buffer = sample.buffer().ok_or(gst::FlowError::Error)?;
                    let map = buffer.map_readable().map_err(|_| gst::FlowError::Error)?;
                    let pts = buffer.pts();
                    let when = {
                        let mut q = captured.lock().unwrap();
                        let i = q.iter().position(|(p, _)| *p == pts);
                        i.and_then(|i| q.drain(..=i).next_back())
                            .map_or_else(Instant::now, |(_, t)| t)
                    };
                    let frame = EncodedFrame {
                        data: map.as_slice().to_vec(),
                        key: !buffer.flags().contains(gst::BufferFlags::DELTA_UNIT),
                        captured: when,
                    };
                    out.send(frame).map_err(|_| gst::FlowError::Flushing)?;
                    Ok(gst::FlowSuccess::Ok)
                })
                .build(),
        );

        pipeline
            .set_state(gst::State::Playing)
            .map_err(|e| format!("GStreamer: {e}"))?;
        let bus = pipeline.bus().ok_or("pipeline has no bus")?;
        std::thread::spawn(move || {
            for msg in bus.iter_timed(gst::ClockTime::NONE) {
                if let gst::MessageView::Error(e) = msg.view() {
                    log::error!("GStreamer: {} ({:?})", e.error(), e.debug());
                    break;
                }
                if let gst::MessageView::Eos(_) = msg.view() {
                    break;
                }
            }
        });
        self.pipeline = Some(pipeline);
        self.encoder = Some((enc, kbps));
        self.cast = Some(cast);
        Ok((w, h))
    }

    fn stop(&mut self) {
        if let Some(p) = self.pipeline.take() {
            let _ = p.set_state(gst::State::Null);
        }
        self.encoder = None;
        self.cast = None; // ends the portal session: the virtual monitor goes away
    }

    fn request_keyframe(&mut self) {
        if let Some((enc, _)) = &self.encoder
            && let Some(pad) = enc.static_pad("sink")
        {
            pad.send_event(
                gst_video::DownstreamForceKeyUnitEvent::builder()
                    .all_headers(true)
                    .build(),
            );
        }
    }

    fn set_bitrate(&mut self, bps: u32) {
        if let Some((enc, kbps)) = &self.encoder
            && enc.has_property("bitrate")
        {
            enc.set_property_from_str("bitrate", &if *kbps { bps / 1000 } else { bps }.to_string());
        }
    }

    fn pointer(&mut self, action: Pointer, x: f32, y: f32) {
        self.send(portal::Input::Pointer(action, x, y));
    }

    fn scroll(&mut self, x: f32, y: f32, dx: f32, dy: f32) {
        self.send(portal::Input::Scroll(x, y, dx, dy));
    }

    fn zoom(&mut self, direction: i8) {
        self.send(portal::Input::Zoom(direction));
    }
}

impl LinuxBackend {
    fn send(&self, input: portal::Input) {
        if let Some(c) = &self.cast {
            let _ = c.input.send(input);
        }
    }
}
