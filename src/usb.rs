//! Raw USB to the tablet in Android Open Accessory (AOA) mode, the fast path: the Mac-app
//! equivalent is Usb.swift. The chosen tablet (matched by USB serial number, so no other
//! Android device is ever touched) is asked to re-enumerate as an accessory, then messages go
//! over its bulk endpoints.
//!
//! Linux needs read/write access to the device (see packaging/linux/70-spanly.rules).
//! Windows needs the WinUSB driver on the accessory interface (see the README).

use crate::link::{Event, Link, LinkKind};
use crate::protocol::{self, Reader, msg};
use crossbeam_channel::{Sender, unbounded};
use rusb::{Context, Device, DeviceHandle, Direction, TransferType, UsbContext};
use std::collections::HashMap;
use std::sync::atomic::{AtomicBool, AtomicU64, AtomicUsize, Ordering};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

const GOOGLE: u16 = 0x18D1;
const ACCESSORY_PRODUCTS: std::ops::RangeInclusive<u16> = 0x2D00..=0x2D05;
const APPLE: u16 = 0x05AC;
/// Vendors that make Android phones/tablets (used together with interface sniffing).
const ANDROID_VENDORS: [u16; 22] = [
    0x18D1, 0x2717, 0x04E8, 0x22B8, 0x2A70, 0x12D1, 0x0BB4, 0x1004, 0x0FCE, 0x19D2, 0x2A45, 0x05C6,
    0x0E8D, 0x1BBB, 0x17EF, 0x0B05, 0x22D9, 0x2D95, 0x2AE5, 0x29A9, 0x1949, 0x2916,
];
const STRINGS: [&str; 6] = [
    "caigenix",
    "Spanly",
    "Spanly second display",
    "1.0",
    "https://github.com/caigenix/spanly",
    "spanly",
];
const TIMEOUT: Duration = Duration::from_secs(1);

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Tablet {
    pub serial: String,
    pub name: String,
    pub accessory_mode: bool,
}

struct Open {
    handle: Arc<DeviceHandle<Context>>,
    tablet: Tablet,
    writer: Sender<Vec<u8>>,
    generation: u64,
}

pub struct UsbLink {
    ctx: Context,
    serial: Box<dyn Fn() -> Option<String> + Send + Sync>,
    events: Sender<Event>,
    open: Mutex<Option<Open>>,
    hello_seen: AtomicBool,
    pending: Arc<AtomicUsize>,
    generation: AtomicU64,
    last_switch: Mutex<HashMap<String, Instant>>,
}

impl UsbLink {
    pub fn start(
        serial: impl Fn() -> Option<String> + Send + Sync + 'static,
        events: Sender<Event>,
    ) -> Option<Arc<Self>> {
        let ctx = Context::new()
            .map_err(|e| log::warn!("libusb unavailable: {e}"))
            .ok()?;
        let link = Arc::new(Self {
            ctx,
            serial: Box::new(serial),
            events,
            open: Mutex::new(None),
            hello_seen: AtomicBool::new(false),
            pending: Arc::new(AtomicUsize::new(0)),
            generation: AtomicU64::new(0),
            last_switch: Mutex::new(HashMap::new()),
        });
        let l = link.clone();
        std::thread::spawn(move || {
            loop {
                if l.open.lock().unwrap().is_none() {
                    l.scan();
                }
                std::thread::sleep(Duration::from_secs(1));
            }
        });
        Some(link)
    }

    /// The tablet the accessory link is talking to (once its app has said HELLO).
    pub fn connected(&self) -> Option<Tablet> {
        if !self.hello_seen.load(Ordering::Relaxed) {
            return None;
        }
        self.open.lock().unwrap().as_ref().map(|o| o.tablet.clone())
    }

    /// Android devices currently plugged in (for `--list-devices`).
    pub fn tablets(&self) -> Vec<Tablet> {
        let Ok(list) = self.ctx.devices() else {
            return vec![];
        };
        list.iter()
            .filter_map(|dev| {
                let d = dev.device_descriptor().ok()?;
                if !looks_like_android(&dev, d.vendor_id(), d.product_id()) {
                    return None;
                }
                let h = dev.open().ok()?;
                let serial = h.read_serial_number_string_ascii(&d).ok()?;
                Some(Tablet {
                    serial,
                    name: display_name(&h, &d),
                    accessory_mode: is_accessory(d.vendor_id(), d.product_id()),
                })
            })
            .collect()
    }

    fn scan(self: &Arc<Self>) {
        let Some(want) = (self.serial)() else { return };
        let Ok(list) = self.ctx.devices() else { return };
        for dev in list.iter() {
            let Ok(d) = dev.device_descriptor() else {
                continue;
            };
            let Ok(h) = dev.open() else { continue };
            if h.read_serial_number_string_ascii(&d).ok().as_deref() != Some(want.as_str()) {
                continue;
            }
            if is_accessory(d.vendor_id(), d.product_id()) {
                self.open_accessory(&dev, h, &d);
            } else {
                self.switch_to_accessory(&h, &want);
            }
            return;
        }
    }

    /// AOA handshake: GET_PROTOCOL (51), SEND_STRING (52) x6, START (53).
    fn switch_to_accessory(&self, h: &DeviceHandle<Context>, serial: &str) {
        let mut last = self.last_switch.lock().unwrap();
        if last
            .get(serial)
            .is_some_and(|t| t.elapsed() < Duration::from_secs(10))
        {
            return; // still re-enumerating
        }
        let mut version = [0u8; 2];
        if h.read_control(0xC0, 51, 0, 0, &mut version, TIMEOUT).ok() != Some(2) || version[0] < 1 {
            log::warn!("tablet does not support USB accessory mode");
            last.insert(serial.into(), Instant::now() + Duration::from_secs(3600));
            return;
        }
        for (i, s) in STRINGS.iter().enumerate() {
            let mut bytes = s.as_bytes().to_vec();
            bytes.push(0);
            let _ = h.write_control(0x40, 52, 0, i as u16, &bytes, TIMEOUT);
        }
        let _ = h.write_control(0x40, 53, 0, 0, &[], TIMEOUT);
        last.insert(serial.into(), Instant::now());
        log::info!("asked the tablet to switch to USB accessory mode");
    }

    fn open_accessory(
        self: &Arc<Self>,
        dev: &Device<Context>,
        h: DeviceHandle<Context>,
        d: &rusb::DeviceDescriptor,
    ) {
        let Ok(cfg) = dev.active_config_descriptor() else {
            return;
        };
        let (mut ep_in, mut ep_out, mut packet) = (0u8, 0u8, 512usize);
        if let Some(alt) = cfg.interfaces().next().and_then(|i| i.descriptors().next()) {
            for ep in alt
                .endpoint_descriptors()
                .filter(|e| e.transfer_type() == TransferType::Bulk)
            {
                match ep.direction() {
                    Direction::In => ep_in = ep.address(),
                    Direction::Out => {
                        ep_out = ep.address();
                        packet = ep.max_packet_size().max(64) as usize;
                    }
                }
            }
        }
        let _ = h.set_auto_detach_kernel_driver(true); // Linux only; harmless elsewhere
        if ep_in == 0 || ep_out == 0 {
            return;
        }
        if let Err(e) = h.claim_interface(0) {
            log::warn!(
                "could not claim the tablet's accessory interface: {e}{}",
                if cfg!(windows) {
                    " (install the WinUSB driver for it, see the README)"
                } else {
                    ""
                }
            );
            return;
        }
        let tablet = Tablet {
            serial: h.read_serial_number_string_ascii(d).unwrap_or_default(),
            name: display_name(&h, d),
            accessory_mode: true,
        };
        let handle = Arc::new(h);
        let generation = self.generation.fetch_add(1, Ordering::Relaxed) + 1;
        let (tx, rx) = unbounded::<Vec<u8>>();
        {
            let (h, link) = (handle.clone(), self.clone());
            std::thread::spawn(move || {
                for mut m in rx {
                    // A transfer that is a multiple of the packet size would need a zero-length
                    // packet to complete on the tablet; pad with a NOP instead.
                    if m.len() % packet == 0 {
                        m.extend(protocol::encode(msg::NOP, &[]));
                    }
                    let mut off = 0;
                    while off < m.len() {
                        match h.write_bulk(ep_out, &m[off..], TIMEOUT) {
                            Ok(n) => off += n,
                            Err(e) => {
                                link.close(generation, &format!("write failed: {e}"));
                                break;
                            }
                        }
                    }
                    link.pending.fetch_sub(1, Ordering::Relaxed);
                }
            });
        }
        self.hello_seen.store(false, Ordering::Relaxed);
        self.pending.store(0, Ordering::Relaxed);
        *self.open.lock().unwrap() = Some(Open {
            handle: handle.clone(),
            tablet,
            writer: tx,
            generation,
        });
        let link = self.clone();
        std::thread::spawn(move || link.read_loop(handle, ep_in, generation));
        let link = self.clone();
        std::thread::spawn(move || link.heartbeat(generation));
    }

    /// The app sends a heartbeat every 0.5 s, so a read only times out when it is gone.
    fn read_loop(&self, h: Arc<DeviceHandle<Context>>, ep: u8, generation: u64) {
        let mut reader = Reader::default();
        let mut buf = vec![0u8; 16384];
        while self.generation.load(Ordering::Relaxed) == generation {
            match h.read_bulk(ep, &mut buf, Duration::from_secs(3)) {
                Ok(n) => {
                    reader.push(&buf[..n]);
                    while let Some((kind, payload)) = reader.next() {
                        if kind == msg::HELLO && !self.hello_seen.swap(true, Ordering::Relaxed) {
                            log::info!("tablet connected over USB accessory");
                            let _ = self.events.send(Event::Connected(LinkKind::Usb));
                        }
                        let _ = self
                            .events
                            .send(Event::Message(LinkKind::Usb, kind, payload));
                    }
                }
                Err(rusb::Error::Timeout) if !self.hello_seen.load(Ordering::Relaxed) => {}
                Err(rusb::Error::Timeout) => {
                    return self.close(generation, "no heartbeat from the app");
                }
                Err(e) => return self.close(generation, &e.to_string()),
            }
        }
    }

    /// Until the app says HELLO keep asking for it (it may hold a session from before the link
    /// reopened); after that a NOP keeps its blocking read responsive.
    fn heartbeat(&self, generation: u64) {
        while self.generation.load(Ordering::Relaxed) == generation {
            if self.pending.load(Ordering::Relaxed) == 0 {
                let kind = if self.hello_seen.load(Ordering::Relaxed) {
                    msg::NOP
                } else {
                    msg::HELLO_REQUEST
                };
                self.enqueue(kind, &[]);
            }
            std::thread::sleep(Duration::from_millis(500));
        }
    }

    fn enqueue(&self, kind: u8, payload: &[u8]) {
        if let Some(o) = self.open.lock().unwrap().as_ref() {
            self.pending.fetch_add(1, Ordering::Relaxed);
            let _ = o.writer.send(protocol::encode(kind, payload));
        }
    }

    fn close(&self, generation: u64, reason: &str) {
        let mut open = self.open.lock().unwrap();
        if open.as_ref().is_none_or(|o| o.generation != generation) {
            return;
        }
        if let Some(o) = open.take() {
            let _ = o.handle.release_interface(0);
        }
        self.generation.fetch_add(1, Ordering::Relaxed);
        if self.hello_seen.swap(false, Ordering::Relaxed) {
            log::info!("USB accessory link closed ({reason})");
            let _ = self.events.send(Event::Disconnected(LinkKind::Usb));
        }
    }
}

impl Link for UsbLink {
    fn kind(&self) -> LinkKind {
        LinkKind::Usb
    }

    fn is_connected(&self) -> bool {
        self.hello_seen.load(Ordering::Relaxed) && self.open.lock().unwrap().is_some()
    }

    fn send(&self, kind: u8, payload: &[u8]) {
        if self.hello_seen.load(Ordering::Relaxed) {
            self.enqueue(kind, payload);
        }
    }
}

fn is_accessory(vendor: u16, product: u16) -> bool {
    vendor == GOOGLE && ACCESSORY_PRODUCTS.contains(&product)
}

/// A known Android vendor, or an adb / MTP / PTP interface.
fn looks_like_android(dev: &Device<Context>, vendor: u16, product: u16) -> bool {
    if vendor == APPLE {
        return false;
    }
    if is_accessory(vendor, product) || ANDROID_VENDORS.contains(&vendor) {
        return true;
    }
    let Ok(cfg) = dev.active_config_descriptor() else {
        return false;
    };
    cfg.interfaces().flat_map(|i| i.descriptors()).any(|alt| {
        let adb =
            alt.class_code() == 0xFF && alt.sub_class_code() == 0x42 && alt.protocol_code() == 0x01;
        adb || alt.class_code() == 0x06 // PTP / MTP
    })
}

fn display_name(h: &DeviceHandle<Context>, d: &rusb::DeviceDescriptor) -> String {
    let maker = h.read_manufacturer_string_ascii(d).unwrap_or_default();
    let product = h
        .read_product_string_ascii(d)
        .unwrap_or_else(|_| "Android device".into());
    if maker.is_empty() || product.to_lowercase().starts_with(&maker.to_lowercase()) {
        product
    } else {
        format!("{maker} {product}")
    }
}
