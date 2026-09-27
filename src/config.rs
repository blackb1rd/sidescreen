//! Settings kept between runs, in a small `key = value` file:
//! Linux `$XDG_CONFIG_HOME/sidescreen/config` (or `~/.config/...`),
//! Windows `%APPDATA%\SideScreen\config`.

use std::collections::BTreeMap;
use std::path::PathBuf;

#[derive(Default)]
pub struct Config {
    values: BTreeMap<String, String>,
}

pub fn dir() -> PathBuf {
    if cfg!(windows) {
        PathBuf::from(std::env::var("APPDATA").unwrap_or_else(|_| ".".into())).join("SideScreen")
    } else if let Ok(x) = std::env::var("XDG_CONFIG_HOME") {
        PathBuf::from(x).join("sidescreen")
    } else {
        PathBuf::from(std::env::var("HOME").unwrap_or_else(|_| ".".into()))
            .join(".config/sidescreen")
    }
}

impl Config {
    pub fn load() -> Self {
        let text = std::fs::read_to_string(dir().join("config")).unwrap_or_default();
        let values = text
            .lines()
            .filter_map(|l| l.split_once('='))
            .map(|(k, v)| (k.trim().to_string(), v.trim().to_string()))
            .collect();
        Self { values }
    }

    pub fn get(&self, key: &str) -> Option<&str> {
        self.values.get(key).map(String::as_str)
    }

    pub fn set(&mut self, key: &str, value: Option<&str>) {
        match value {
            Some(v) => self.values.insert(key.into(), v.into()),
            None => self.values.remove(key),
        };
        let text: String = self
            .values
            .iter()
            .map(|(k, v)| format!("{k} = {v}\n"))
            .collect();
        let _ = std::fs::create_dir_all(dir());
        if let Err(e) = std::fs::write(dir().join("config"), text) {
            log::warn!("could not save settings: {e}");
        }
    }

    /// The Wi-Fi pairing secret handed to tablets over USB (made on first use).
    pub fn wifi_secret(&mut self) -> [u8; 32] {
        if let Some(s) = self.get("wifi_secret").and_then(from_hex)
            && let Ok(a) = <[u8; 32]>::try_from(s.as_slice())
        {
            return a;
        }
        self.reset_wifi_secret()
    }

    pub fn reset_wifi_secret(&mut self) -> [u8; 32] {
        let s = crate::crypto::random_bytes::<32>();
        self.set("wifi_secret", Some(&to_hex(&s)));
        s
    }
}

pub fn to_hex(b: &[u8]) -> String {
    b.iter().map(|x| format!("{x:02x}")).collect()
}

fn from_hex(s: &str) -> Option<Vec<u8>> {
    (0..s.len())
        .step_by(2)
        .map(|i| u8::from_str_radix(s.get(i..i + 2)?, 16).ok())
        .collect()
}
