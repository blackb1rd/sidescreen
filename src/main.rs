//! SideScreen for Windows and Linux: use an Android tablet as a second display over USB or
//! Wi-Fi. The tablet runs the SideScreen app; the Mac version is github.com/blackb1rd/sidescreen.

mod adb;
mod backend;
mod beacon;
mod config;
mod crypto;
mod flow;
mod link;
mod platform;
mod protocol;
mod session;
mod usb;
mod wifi;

use backend::Mode;
use config::Config;
use std::sync::{Arc, Mutex};

const HELP: &str = "\
usage: sidescreen [options]

  --mirror             show the same picture as the main screen (default: extend, a second screen)
  --wifi on|off        let tablets that were plugged in once connect over Wi-Fi (saved)
  --forget-pairing     new Wi-Fi secret: every tablet must be plugged in again
  --list-devices       list connected Android devices and exit
  --device SERIAL      use this device as the tablet (saved; see --list-devices)
  --fps N              frame rate (default 60)
  --bitrate MBPS       starting/maximum bitrate (default: 20 USB, 12 Wi-Fi, 6 adb)
  --no-adb             don't manage adb (reverse forwarding, opening the app)
  --no-usb             don't use USB accessory mode (e.g. to try Wi-Fi with the cable in)
  --stats              log frame rate, latency and bitrate
  --test-pattern       stream a test pattern instead of the desktop (builds with that feature)
";

fn main() {
    env_logger::Builder::from_env(env_logger::Env::default().default_filter_or("info"))
        .format_timestamp_secs()
        .init();
    let mut args = std::env::args().skip(1);
    let mut cfg = Config::load();
    let (
        mut mode,
        mut fps,
        mut bitrate,
        mut stats,
        mut manage_adb,
        mut test_pattern,
        mut list,
        mut use_usb,
    ) = (Mode::Extend, 60, None, false, true, false, false, true);
    while let Some(a) = args.next() {
        match a.as_str() {
            "--mirror" => mode = Mode::Mirror,
            "--wifi" => cfg.set("wifi", args.next().as_deref().filter(|v| *v == "on")),
            "--forget-pairing" => {
                cfg.reset_wifi_secret();
                println!("Wi-Fi pairing reset: plug each tablet in once to pair it again.");
            }
            "--list-devices" => list = true,
            "--device" => cfg.set("device", args.next().as_deref()),
            "--fps" => fps = args.next().and_then(|v| v.parse().ok()).unwrap_or(fps),
            "--bitrate" => bitrate = args.next().and_then(|v| v.parse().ok()),
            "--no-adb" => manage_adb = false,
            "--no-usb" => use_usb = false,
            "--stats" => stats = true,
            "--test-pattern" => test_pattern = true,
            "-h" | "--help" => return print!("{HELP}"),
            other => {
                eprintln!("unknown option {other}\n\n{HELP}");
                std::process::exit(2);
            }
        }
    }

    let config = Arc::new(Mutex::new(cfg));
    let (tx, rx) = crossbeam_channel::unbounded();
    let adb = Arc::new(adb::AdbLink::start(manage_adb, tx.clone()));
    // The tablet: the one chosen with --device, else the one adb sees.
    let usb = if !use_usb && !list {
        None
    } else {
        let (config, adb) = (config.clone(), adb.clone());
        usb::UsbLink::start(
            move || {
                config
                    .lock()
                    .unwrap()
                    .get("device")
                    .map(str::to_string)
                    .or_else(|| adb.serial())
            },
            tx.clone(),
        )
    };

    if list {
        match &usb {
            Some(u) => u.tablets().iter().for_each(|t| {
                println!(
                    "{}  {}{}",
                    t.serial,
                    t.name,
                    if t.accessory_mode {
                        "  (in accessory mode)"
                    } else {
                        ""
                    }
                )
            }),
            None => eprintln!("USB unavailable"),
        }
        return;
    }

    let host_name = host_name();
    let wifi = if config.lock().unwrap().get("wifi") == Some("on") {
        let config = config.clone();
        wifi::WifiLink::start(
            &host_name,
            move || config.lock().unwrap().wifi_secret(),
            tx.clone(),
        )
    } else {
        None
    };

    // Nothing chosen and exactly one Android device plugged in: that's the tablet.
    if let Some(u) = usb.clone() {
        let (config, adb) = (config.clone(), adb.clone());
        std::thread::spawn(move || {
            loop {
                if config.lock().unwrap().get("device").is_none() && adb.serial().is_none() {
                    if let [t] = u.tablets().as_slice() {
                        log::info!(
                            "using {} ({}) as the tablet; change with --device",
                            t.name,
                            t.serial
                        );
                        config.lock().unwrap().set("device", Some(&t.serial));
                    }
                }
                std::thread::sleep(std::time::Duration::from_secs(3));
            }
        });
    }

    let backend = match backend::create(test_pattern) {
        Ok(b) => b,
        Err(e) => {
            log::error!("{e}");
            std::process::exit(1);
        }
    };
    log::info!("waiting for the tablet ({:?} mode)", mode);
    let opts = session::Options {
        mode,
        fps,
        bitrate,
        stats,
        host_name,
    };
    let links = session::Links { usb, wifi, adb };
    session::Controller::new(opts, links, backend, config).run(rx);
}

fn host_name() -> String {
    std::env::var("COMPUTERNAME")
        .or_else(|_| std::fs::read_to_string("/etc/hostname").map(|s| s.trim().to_string()))
        .or_else(|_| std::env::var("HOSTNAME"))
        .ok()
        .filter(|s| !s.is_empty())
        .unwrap_or_else(|| "SideScreen".into())
}
