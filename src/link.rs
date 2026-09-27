//! Links to the tablet. Each one reports to the session through a shared event channel and
//! sends through its own writer thread, so callers never block on the connection.

use crate::protocol::{self, Reader};
use crossbeam_channel::{Receiver, Sender, unbounded};
use std::io::{Read, Write};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum LinkKind {
    Usb,
    Wifi,
    Adb,
}

pub enum Event {
    /// The tablet's app said HELLO on this link.
    Connected(LinkKind),
    Message(LinkKind, u8, Vec<u8>),
    Disconnected(LinkKind),
}

pub trait Link: Send + Sync {
    fn kind(&self) -> LinkKind;
    fn is_connected(&self) -> bool;
    fn send(&self, kind: u8, payload: &[u8]);
}

/// A connected byte stream to the tablet (TCP through adb, or decrypted Wi-Fi): a writer
/// thread for outgoing messages and a reader that turns bytes into events.
pub struct Session {
    kind: LinkKind,
    tx: Sender<Vec<u8>>,
    pub hello_seen: Arc<AtomicBool>,
    pub open: Arc<AtomicBool>,
}

impl Session {
    /// `write` gets each encoded message (a TCP stream, or an encrypting wrapper).
    pub fn start(
        kind: LinkKind,
        mut write: impl FnMut(&[u8]) -> std::io::Result<()> + Send + 'static,
    ) -> Self {
        let (tx, rx): (Sender<Vec<u8>>, Receiver<Vec<u8>>) = unbounded();
        let open = Arc::new(AtomicBool::new(true));
        let o = open.clone();
        std::thread::spawn(move || {
            for m in rx {
                if o.load(Ordering::Relaxed) && write(&m).is_err() {
                    o.store(false, Ordering::Relaxed);
                }
            }
        });
        Self {
            kind,
            tx,
            hello_seen: Arc::new(AtomicBool::new(false)),
            open,
        }
    }

    pub fn send(&self, kind: u8, payload: &[u8]) {
        if !self.open.load(Ordering::Relaxed) {
            return;
        }
        let _ = self.tx.send(protocol::encode(kind, payload));
    }

    /// Feed received bytes; emits events for complete messages.
    pub fn received(&self, reader: &mut Reader, data: &[u8], events: &Sender<Event>) {
        reader.push(data);
        while let Some((kind, payload)) = reader.next() {
            if kind == protocol::msg::HELLO && !self.hello_seen.swap(true, Ordering::Relaxed) {
                let _ = events.send(Event::Connected(self.kind));
            }
            let _ = events.send(Event::Message(self.kind, kind, payload));
        }
        if reader.skipped > 0 {
            log::info!(
                "{:?}: resynchronised the stream (skipped {} bytes)",
                self.kind,
                reader.skipped
            );
            reader.skipped = 0;
        }
    }

    pub fn close(&self, events: &Sender<Event>) {
        self.open.store(false, Ordering::Relaxed);
        if self.hello_seen.swap(false, Ordering::Relaxed) {
            let _ = events.send(Event::Disconnected(self.kind));
        }
    }
}

/// Runs a plain TCP session until the stream ends (used for adb's forwarded connections).
pub fn serve_tcp(
    kind: LinkKind,
    stream: std::net::TcpStream,
    slot: Arc<Mutex<Option<Arc<Session>>>>,
    events: Sender<Event>,
) {
    let _ = stream.set_nodelay(true);
    let _ = stream.set_read_timeout(Some(std::time::Duration::from_secs(4)));
    let Ok(mut writer) = stream.try_clone() else {
        return;
    };
    let session = Arc::new(Session::start(kind, move |m| writer.write_all(m)));
    if let Some(old) = slot.lock().unwrap().replace(session.clone()) {
        old.close(&events);
    }
    let mut reader = Reader::default();
    let mut buf = vec![0u8; 1 << 16];
    let mut stream = stream;
    while session.open.load(Ordering::Relaxed) {
        match stream.read(&mut buf) {
            Ok(0) | Err(_) => break, // closed, or 4 s without even a heartbeat
            Ok(n) => session.received(&mut reader, &buf[..n], &events),
        }
    }
    session.close(&events);
    let mut s = slot.lock().unwrap();
    if s.as_ref().is_some_and(|x| Arc::ptr_eq(x, &session)) {
        *s = None;
    }
}
