//! The tablet over Wi-Fi: a Bonjour-advertised TCP listener (`_spanly._tcp`, port 27184)
//! whose traffic is encrypted with keys derived from the secret paired over USB (crypto.rs).
//! Same protocol as the Mac app's WifiLink.swift.

use crate::crypto::{self, MAGIC, NONCE_SIZE};
use crate::link::{Event, Link, LinkKind, Session};
use crate::protocol::Reader;
use crossbeam_channel::Sender;
use mdns_sd::{ServiceDaemon, ServiceInfo};
use std::io::{Read, Write};
use std::net::{TcpListener, TcpStream};
use std::sync::atomic::Ordering;
use std::sync::{Arc, Mutex};
use std::time::Duration;

pub const PORT: u16 = 27184;
const SERVICE: &str = "_spanly._tcp.local.";

pub struct WifiLink {
    session: Arc<Mutex<Option<Arc<Session>>>>,
    _mdns: ServiceDaemon,
}

impl WifiLink {
    /// Start listening and advertising. `secret` is read for every new connection.
    pub fn start(
        host_name: &str,
        secret: impl Fn() -> [u8; 32] + Send + Sync + 'static,
        events: Sender<Event>,
    ) -> Option<Self> {
        let mdns = ServiceDaemon::new()
            .map_err(|e| log::warn!("Bonjour unavailable: {e}"))
            .ok()?;
        let info = ServiceInfo::new(
            SERVICE,
            host_name,
            &format!("{}.local.", sanitize(host_name)),
            "",
            PORT,
            None::<std::collections::HashMap<String, String>>,
        )
        .map_err(|e| log::warn!("Bonjour: {e}"))
        .ok()?
        .enable_addr_auto();
        if let Err(e) = mdns.register(info) {
            log::warn!("could not advertise over Bonjour: {e}");
        }

        let session: Arc<Mutex<Option<Arc<Session>>>> = Arc::default();
        let secret = Arc::new(secret);
        // Both stacks: Android may resolve the host to an IPv6 address.
        let listeners: Vec<TcpListener> = ["[::]", "0.0.0.0"]
            .iter()
            .filter_map(|a| TcpListener::bind(format!("{a}:{PORT}")).ok())
            .collect();
        if listeners.is_empty() {
            log::warn!("could not listen for Wi-Fi connections on port {PORT}");
            return None;
        }
        for listener in listeners {
            let (session, secret, events) = (session.clone(), secret.clone(), events.clone());
            std::thread::spawn(move || {
                for stream in listener.incoming().flatten() {
                    let (session, secret, events) =
                        (session.clone(), secret.clone(), events.clone());
                    std::thread::spawn(move || serve(stream, &secret(), session, events));
                }
            });
        }
        log::info!("accepting paired tablets over Wi-Fi on port {PORT} as \"{host_name}\"");
        // For tablets Bonjour doesn't reach (e.g. this computer is on the tablet's hotspot).
        crate::beacon::start(host_name, PORT);
        Some(Self {
            session,
            _mdns: mdns,
        })
    }
}

impl Link for WifiLink {
    fn kind(&self) -> LinkKind {
        LinkKind::Wifi
    }

    fn is_connected(&self) -> bool {
        self.session
            .lock()
            .unwrap()
            .as_ref()
            .is_some_and(|s| s.hello_seen.load(Ordering::Relaxed))
    }

    fn send(&self, kind: u8, payload: &[u8]) {
        if let Some(s) = self.session.lock().unwrap().as_ref() {
            s.send(kind, payload);
        }
    }
}

/// Handshake, then decrypt records into messages until the connection ends.
fn serve(
    mut stream: TcpStream,
    secret: &[u8; 32],
    slot: Arc<Mutex<Option<Arc<Session>>>>,
    events: Sender<Event>,
) {
    let _ = stream.set_nodelay(true);
    // The tablet sends a heartbeat every 0.5 s; 4 s of silence means it's gone.
    let _ = stream.set_read_timeout(Some(Duration::from_secs(4)));
    let mut hello = [0u8; 4 + NONCE_SIZE];
    if stream.read_exact(&mut hello).is_err() || &hello[..4] != MAGIC {
        return;
    }
    let server_nonce = crypto::random_bytes::<NONCE_SIZE>();
    if stream.write_all(&server_nonce).is_err() {
        return;
    }
    let (mut seal, mut open) = crypto::server_session(secret, &hello[4..], &server_nonce);
    let Ok(mut out) = stream.try_clone() else {
        return;
    };
    let session = Arc::new(Session::start(LinkKind::Wifi, move |m| {
        let sealed = seal.seal(m);
        let mut record = (sealed.len() as u32).to_be_bytes().to_vec();
        record.extend(sealed);
        out.write_all(&record)
    }));
    if let Some(old) = slot.lock().unwrap().replace(session.clone()) {
        old.close(&events);
    }
    let mut reader = Reader::default();
    loop {
        let mut len = [0u8; 4];
        if stream.read_exact(&mut len).is_err() {
            break;
        }
        let len = u32::from_be_bytes(len) as usize;
        if len > crypto::MAX_RECORD {
            break;
        }
        let mut record = vec![0u8; len];
        if stream.read_exact(&mut record).is_err() {
            break;
        }
        let Some(plain) = open.open(&record) else {
            log::warn!("Wi-Fi: a device that isn't paired tried to connect");
            break;
        };
        if !session.hello_seen.load(Ordering::Relaxed)
            && plain.len() > 1
            && plain[1] == crate::protocol::msg::HELLO
        {
            log::info!("tablet connected over Wi-Fi");
        }
        session.received(&mut reader, &plain, &events);
    }
    session.close(&events);
    let mut s = slot.lock().unwrap();
    if s.as_ref().is_some_and(|x| Arc::ptr_eq(x, &session)) {
        *s = None;
    }
}

fn sanitize(name: &str) -> String {
    name.chars()
        .map(|c| if c.is_ascii_alphanumeric() { c } else { '-' })
        .collect()
}
