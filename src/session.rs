//! Ties it together: tablet sessions on whichever link is best, the stream, flow control,
//! adaptive bitrate and input. The equivalent of the Mac app's Controller.

use crate::adb::AdbLink;
use crate::backend::{self, Backend, EncodedFrame, Mode, Pointer, StreamRequest};
use crate::config::Config;
use crate::flow::{self, Flow};
use crate::link::{Event, Link, LinkKind};
use crate::protocol::{self, f32_at, msg, u32_at};
use crate::usb::UsbLink;
use crate::wifi::WifiLink;
use crossbeam_channel::{Receiver, Sender, select, unbounded};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

pub struct Options {
    pub mode: Mode,
    pub fps: u32,
    /// Fixed starting/maximum bitrate in Mbit/s; default depends on the link.
    pub bitrate: Option<f64>,
    pub stats: bool,
    pub host_name: String,
}

pub struct Links {
    pub usb: Option<Arc<UsbLink>>,
    pub wifi: Option<WifiLink>,
    pub adb: Arc<AdbLink>,
}

struct Hello {
    width: u32,
    height: u32,
    max_width: u32,
    max_height: u32,
}

pub struct Controller {
    opts: Options,
    links: Links,
    backend: Box<dyn Backend>,
    config: Arc<Mutex<Config>>,
    flow: Arc<Flow>,
    viewing: Arc<AtomicBool>,
    frames: (Sender<EncodedFrame>, Receiver<EncodedFrame>),
    hello: Option<Hello>,
    size: Option<(u32, u32)>,
    stream_link: Option<LinkKind>,
    waiting_for_key: bool,
    bitrate: f64,
    lost_since: Option<Instant>,
    stats: (u32, f64, u64), // frames, latency ms sum, bytes
}

impl Controller {
    pub fn new(
        opts: Options,
        links: Links,
        backend: Box<dyn Backend>,
        config: Arc<Mutex<Config>>,
    ) -> Self {
        Self {
            opts,
            links,
            backend,
            config,
            flow: Arc::default(),
            viewing: Arc::new(AtomicBool::new(true)),
            frames: unbounded(),
            hello: None,
            size: None,
            stream_link: None,
            waiting_for_key: false,
            bitrate: 0.0,
            lost_since: None,
            stats: (0, 0.0, 0),
        }
    }

    /// Raw USB is best, then Wi-Fi, then TCP through adb.
    fn link(&self) -> &dyn Link {
        if let Some(u) = &self.links.usb
            && u.is_connected()
        {
            return u.as_ref();
        }
        if let Some(w) = &self.links.wifi
            && w.is_connected()
        {
            return w;
        }
        self.links.adb.as_ref()
    }

    fn max_bitrate(&self, kind: LinkKind) -> f64 {
        self.opts.bitrate.unwrap_or(match kind {
            LinkKind::Usb => 20.0,
            LinkKind::Wifi => 12.0,
            LinkKind::Adb => 6.0, // adb over USB tops out around 10-15 Mbit/s
        })
    }

    pub fn run(mut self, events: Receiver<Event>) {
        let tick = crossbeam_channel::tick(Duration::from_secs(1));
        let mut ticks = 0u64;
        loop {
            let frames = self.frames.1.clone();
            select! {
                recv(events) -> ev => match ev {
                    Ok(e) => self.event(e),
                    Err(_) => return,
                },
                recv(frames) -> f => if let Ok(f) = f { self.send_frame(f) },
                recv(tick) -> _ => {
                    ticks += 1;
                    self.every_second(ticks);
                }
            }
        }
    }

    fn event(&mut self, e: Event) {
        match e {
            Event::Connected(kind) => log::info!("tablet connected over {kind:?}"),
            Event::Disconnected(kind) => {
                log::info!("tablet disconnected from {kind:?}");
                if !self.link().is_connected() {
                    self.lost_since = Some(Instant::now());
                }
            }
            Event::Message(kind, t, p) => {
                // Only the link we stream on drives the session (a stale one may linger briefly).
                if kind != self.link().kind() {
                    return;
                }
                self.message(kind, t, &p);
            }
        }
    }

    fn message(&mut self, kind: LinkKind, t: u8, p: &[u8]) {
        match t {
            msg::HELLO => self.hello(kind, p),
            msg::ACK => {
                if let Some(l) = self.flow.acked(u32_at(p, 0)) {
                    self.stats.0 += 1;
                    self.stats.1 += l.as_secs_f64() * 1000.0;
                }
            }
            msg::VIEWING => {
                let on = p[0] != 0;
                if on != self.viewing.swap(on, Ordering::Relaxed) && on {
                    self.restart_stream(); // back in front: new keyframe
                }
            }
            msg::TOUCH => {
                let action = match p[0] {
                    0 => Pointer::Down,
                    1 => Pointer::Move,
                    2 => Pointer::Up,
                    _ => Pointer::RightClick,
                };
                self.backend.pointer(action, f32_at(p, 1), f32_at(p, 5));
            }
            msg::PEN => {
                let action = match (p[0], p[1] & 1 != 0) {
                    (0, _) => Pointer::Hover,
                    (1, true) => Pointer::RightClick,
                    (1, false) => Pointer::Down,
                    (2, _) => Pointer::Move,
                    _ => Pointer::Up,
                };
                self.backend.pointer(action, f32_at(p, 2), f32_at(p, 6));
            }
            msg::SCROLL => {
                self.backend
                    .scroll(f32_at(p, 3), f32_at(p, 7), f32_at(p, 11), f32_at(p, 15))
            }
            msg::ZOOM => self.backend.zoom(p[0] as i8),
            _ => {} // tablet-screen sharing is Mac-only for now
        }
    }

    fn hello(&mut self, kind: LinkKind, p: &[u8]) {
        let hello = Hello {
            width: u32_at(p, 0),
            height: u32_at(p, 4),
            max_width: if p.len() >= 24 { u32_at(p, 16) } else { 0 },
            max_height: if p.len() >= 24 { u32_at(p, 20) } else { 0 },
        };
        self.lost_since = None;
        // Plugged in (raw USB or adb over the cable) = trusted: pair for Wi-Fi.
        if kind != LinkKind::Wifi {
            self.remember_and_pair();
        }
        let same = self
            .hello
            .as_ref()
            .is_some_and(|h| (h.width, h.height) == (hello.width, hello.height));
        self.hello = Some(hello);
        if same && self.size.is_some() && self.stream_link == Some(kind) {
            self.restart_stream(); // e.g. the tablet resynchronised: SIZE + keyframe again
        } else {
            self.start_stream(kind);
        }
    }

    /// Remember the tablet (on raw USB) and hand over the Wi-Fi secret.
    fn remember_and_pair(&mut self) {
        let mut cfg = self.config.lock().unwrap();
        if let Some(t) = self.links.usb.as_ref().and_then(|u| u.connected())
            && cfg.get("device") != Some(t.serial.as_str())
        {
            cfg.set("device", Some(&t.serial));
            cfg.set("device_name", Some(&t.name));
        }
        let mut pair = cfg.wifi_secret().to_vec();
        pair.extend(self.opts.host_name.as_bytes().iter().take(200));
        drop(cfg);
        self.link().send(msg::PAIR, &pair);
    }

    fn start_stream(&mut self, kind: LinkKind) {
        let Some(h) = &self.hello else { return };
        self.backend.stop();
        self.bitrate = self.max_bitrate(kind);
        let req = StreamRequest {
            mode: self.opts.mode,
            tablet_width: h.width,
            tablet_height: h.height,
            max_width: h.max_width,
            max_height: h.max_height,
            fps: self.opts.fps,
            bitrate: (self.bitrate * 1e6) as u32,
        };
        let (flow, viewing) = (self.flow.clone(), self.viewing.clone());
        let gate: backend::Gate =
            Arc::new(move || viewing.load(Ordering::Relaxed) && flow.may_send());
        self.frames = unbounded();
        match self.backend.start(&req, gate, self.frames.0.clone()) {
            Ok(size) => {
                log::info!(
                    "streaming {}x{} ({:?}) over {kind:?} at {:.0} Mbit/s",
                    size.0,
                    size.1,
                    self.opts.mode,
                    self.bitrate
                );
                self.size = Some(size);
                self.stream_link = Some(kind);
                self.restart_stream();
            }
            Err(e) => {
                log::error!("could not start the stream: {e}");
                self.size = None;
            }
        }
    }

    /// Tell the tablet the geometry and start over from a keyframe.
    fn restart_stream(&mut self) {
        let Some((w, h)) = self.size else { return };
        let link = self.link();
        let mut p = Vec::with_capacity(12);
        for v in [w, h, 0 /* H.264 */] {
            p.extend_from_slice(&v.to_be_bytes());
        }
        link.send(msg::SIZE, &p);
        link.send(msg::DISPLAY, &[1]);
        self.flow
            .reset(if link.kind() == LinkKind::Wifi { 6 } else { 3 });
        self.waiting_for_key = true;
        self.backend.request_keyframe();
    }

    fn send_frame(&mut self, f: EncodedFrame) {
        if !self.link().is_connected() || self.size.is_none() || (!f.key && self.waiting_for_key) {
            return;
        }
        let nals = protocol::nal_units(&f.data);
        // Parameter sets go in CONFIG; the frame itself carries only picture data.
        let config: Vec<u8> = nals
            .iter()
            .filter(|n| f.key && matches!(protocol::nal_type(n), 7 | 8))
            .flat_map(|n| [&[0, 0, 0, 1][..], n].concat())
            .collect();
        let id = self.flow.register(f.captured);
        let mut p = vec![u8::from(f.key)];
        p.extend_from_slice(&id.to_be_bytes());
        for n in nals
            .iter()
            .filter(|n| !matches!(protocol::nal_type(n), 7..=9))
        {
            p.extend_from_slice(&[0, 0, 0, 1]);
            p.extend_from_slice(n);
        }
        if f.key {
            self.waiting_for_key = false;
        }
        self.stats.2 += p.len() as u64;
        let link = self.link();
        if !config.is_empty() {
            link.send(msg::CONFIG, &config);
        }
        link.send(msg::FRAME, &p);
    }

    fn every_second(&mut self, ticks: u64) {
        // Stop streaming 10 s after the tablet left (brief reconnects keep the display).
        if let Some(t) = self.lost_since
            && t.elapsed() > Duration::from_secs(10)
            && !self.link().is_connected()
        {
            log::info!("tablet gone; stopped streaming");
            self.backend.stop();
            self.size = None;
            self.hello = None;
            self.lost_since = None;
        }
        if self.size.is_some() && self.link().is_connected() {
            let w = self.flow.take_window();
            if let Some(b) =
                flow::adapt_bitrate(self.bitrate, self.max_bitrate(self.link().kind()), &w)
            {
                self.bitrate = b;
                self.backend.set_bitrate((b * 1e6) as u32);
                if self.opts.stats {
                    log::info!(
                        "bitrate -> {b:.1} Mbit/s (latency {:.0} ms, {} skipped)",
                        w.avg_latency_ms,
                        w.skipped
                    );
                }
            }
        }
        if self.opts.stats && ticks % 5 == 0 {
            let (n, lat, bytes) = std::mem::take(&mut self.stats);
            if n > 0 {
                log::info!(
                    "{:.0} fps, capture-to-decoded avg {:.1} ms, {:.1} Mbit/s",
                    n as f64 / 5.0,
                    lat / n as f64,
                    bytes as f64 * 8.0 / 5e6
                );
            }
        }
    }
}
