//! Encryption for the Wi-Fi link, identical to the Mac (WifiCrypto.swift) and Android
//! (SecureChannel.kt) implementations: HKDF-SHA256 per-direction keys from the pairing
//! secret and both nonces, then AES-256-GCM records with a per-direction counter nonce.

use aes_gcm::aead::Aead;
use aes_gcm::{Aes256Gcm, KeyInit, Nonce};
use hkdf::Hkdf;
use sha2::Sha256;

pub const MAGIC: &[u8; 4] = b"SSW1";
pub const NONCE_SIZE: usize = 16;
pub const MAX_RECORD: usize = 20 << 20;

fn derive(secret: &[u8], salt: &[u8], info: &str) -> [u8; 32] {
    let mut out = [0u8; 32];
    Hkdf::<Sha256>::new(Some(salt), secret)
        .expand(info.as_bytes(), &mut out)
        .expect("32 bytes is a valid HKDF-SHA256 length");
    out
}

/// One direction of an encrypted session.
pub struct Direction {
    cipher: Aes256Gcm,
    counter: u64,
}

impl Direction {
    fn new(key: [u8; 32]) -> Self {
        Self {
            cipher: Aes256Gcm::new(&key.into()),
            counter: 0,
        }
    }

    fn nonce(&mut self) -> Nonce<aes_gcm::aead::consts::U12> {
        let mut n = [0u8; 12];
        n[4..].copy_from_slice(&self.counter.to_be_bytes());
        self.counter += 1;
        n.into()
    }

    /// ciphertext || tag
    pub fn seal(&mut self, plain: &[u8]) -> Vec<u8> {
        let nonce = self.nonce();
        self.cipher
            .encrypt(&nonce, plain)
            .expect("AES-GCM encryption cannot fail")
    }

    pub fn open(&mut self, record: &[u8]) -> Option<Vec<u8>> {
        let nonce = self.nonce();
        self.cipher.decrypt(&nonce, record).ok()
    }
}

/// Keys for the host (server) side of a session.
pub fn server_session(
    secret: &[u8],
    client_nonce: &[u8],
    server_nonce: &[u8],
) -> (Direction, Direction) {
    let salt = [client_nonce, server_nonce].concat();
    let send = derive(secret, &salt, "sidescreen s2c");
    let receive = derive(secret, &salt, "sidescreen c2s");
    (Direction::new(send), Direction::new(receive))
}

pub fn random_bytes<const N: usize>() -> [u8; N] {
    let mut b = [0u8; N];
    rand::fill(&mut b);
    b
}

#[cfg(test)]
mod tests {
    use super::*;

    fn hex(b: &[u8]) -> String {
        b.iter().map(|x| format!("{x:02x}")).collect()
    }

    // The same vectors are checked by the Mac (WifiCryptoTests) and Android (SecureChannelTest).
    fn vectors() -> (Vec<u8>, Vec<u8>, Vec<u8>) {
        let secret: Vec<u8> = (1..=32).collect();
        let cn: Vec<u8> = (0..16).map(|i| 0xA0 + i).collect();
        let sn: Vec<u8> = (0..16).map(|i| 0xB0 + i).collect();
        (secret, cn, sn)
    }

    #[test]
    fn derives_the_same_keys_as_mac_and_android() {
        let (secret, cn, sn) = vectors();
        let salt = [cn, sn].concat();
        assert_eq!(
            hex(&derive(&secret, &salt, "sidescreen c2s")),
            "dad5e4f21f47255de3fbe0fc1523607eea3e8b80ef8044f884b03fc116f02000"
        );
        assert_eq!(
            hex(&derive(&secret, &salt, "sidescreen s2c")),
            "e44762191fb04c1f1075dd4b6a18c8250bf19da566626b76ad1c8a1e1cd366c1"
        );
    }

    #[test]
    fn seals_like_the_mac() {
        let (secret, cn, sn) = vectors();
        let (mut send, _) = server_session(&secret, &cn, &sn);
        for _ in 0..5 {
            send.seal(b"skip"); // counter 0..4
        }
        assert_eq!(
            hex(&send.seal(b"hello tablet")),
            "82022662e775c2e0a496d1761b53c7ec67538e98813b69a8d2e73c6f"
        );
    }

    #[test]
    fn round_trips_and_rejects_tampering() {
        let key = random_bytes::<32>();
        let mut a = Direction::new(key);
        let mut b = Direction::new(key);
        let mut sealed = a.seal(b"frame");
        assert_eq!(b.open(&sealed).as_deref(), Some(&b"frame"[..]));
        sealed = a.seal(b"frame");
        sealed[0] ^= 1;
        assert_eq!(b.open(&sealed), None);
    }
}
