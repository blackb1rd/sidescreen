//! The slow fallback: TCP on 127.0.0.1:27183, which `adb reverse` exposes to the tablet over
//! USB. If adb is installed we also keep the reverse forwarding up, open the app, and learn the
//! tablet's serial number (so the USB accessory link knows which device is ours).

use crate::link::{self, Event, Link, LinkKind, Session};
use crossbeam_channel::Sender;
use std::net::TcpListener;
use std::process::Command;
use std::sync::atomic::Ordering;
use std::sync::{Arc, Mutex};
use std::time::Duration;

pub const PORT: u16 = 27183;
const APP: &str = "dev.blackb1rd.sidescreen/.MainActivity";

pub struct AdbLink {
    session: Arc<Mutex<Option<Arc<Session>>>>,
    serial: Arc<Mutex<Option<String>>>,
}

impl AdbLink {
    pub fn start(manage_adb: bool, events: Sender<Event>) -> Self {
        let session: Arc<Mutex<Option<Arc<Session>>>> = Arc::default();
        let serial: Arc<Mutex<Option<String>>> = Arc::default();
        match TcpListener::bind(("127.0.0.1", PORT)) {
            Ok(listener) => {
                let (session, events) = (session.clone(), events.clone());
                std::thread::spawn(move || {
                    for stream in listener.incoming().flatten() {
                        let (session, events) = (session.clone(), events.clone());
                        std::thread::spawn(move || {
                            link::serve_tcp(LinkKind::Adb, stream, session, events)
                        });
                    }
                });
            }
            Err(e) => log::warn!(
                "could not listen on 127.0.0.1:{PORT} ({e}); is SideScreen already running?"
            ),
        }
        if manage_adb && let Some(adb) = find_adb() {
            let serial = serial.clone();
            std::thread::spawn(move || watch(&adb, &serial));
        }
        Self { session, serial }
    }

    /// USB serial of the tablet adb sees, if any.
    pub fn serial(&self) -> Option<String> {
        self.serial.lock().unwrap().clone()
    }
}

impl Link for AdbLink {
    fn kind(&self) -> LinkKind {
        LinkKind::Adb
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

fn find_adb() -> Option<String> {
    let exe = if cfg!(windows) { "adb.exe" } else { "adb" };
    let mut candidates: Vec<std::path::PathBuf> = std::env::var_os("PATH")
        .map(|p| std::env::split_paths(&p).map(|d| d.join(exe)).collect())
        .unwrap_or_default();
    for var in ["ANDROID_HOME", "ANDROID_SDK_ROOT"] {
        if let Some(sdk) = std::env::var_os(var) {
            candidates.push(
                std::path::PathBuf::from(sdk)
                    .join("platform-tools")
                    .join(exe),
            );
        }
    }
    candidates
        .into_iter()
        .find(|p| p.is_file())
        .map(|p| p.to_string_lossy().into_owned())
}

fn run(adb: &str, args: &[&str]) -> String {
    Command::new(adb)
        .args(args)
        .output()
        .map(|o| String::from_utf8_lossy(&o.stdout).trim().to_string())
        .unwrap_or_default()
}

/// Keep `adb reverse` alive across replugs, open the app once per connection, remember the serial.
fn watch(adb: &str, serial: &Mutex<Option<String>>) {
    let mut ready = false;
    loop {
        if run(adb, &["get-state"]) != "device" {
            ready = false;
        } else {
            let s = run(adb, &["get-serialno"]);
            if !s.is_empty() {
                *serial.lock().unwrap() = Some(s);
            }
            if !run(adb, &["reverse", "--list"]).contains(&format!("tcp:{PORT}")) {
                run(
                    adb,
                    &["reverse", &format!("tcp:{PORT}"), &format!("tcp:{PORT}")],
                );
                ready = false;
            }
            if !ready {
                run(adb, &["shell", "am", "start", "-n", APP]);
                ready = true;
            }
        }
        std::thread::sleep(Duration::from_secs(2));
    }
}
