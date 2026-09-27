import CryptoKit
import Foundation

/// Encryption for the Wi-Fi link (see PROTOCOL.md, "Wi-Fi").
///
/// The tablet receives a 32-byte pairing secret over USB. Each Wi-Fi session starts with both
/// sides exchanging a random 16-byte nonce; HKDF-SHA256 turns secret + nonces into one key per
/// direction. Every record is AES-256-GCM with a per-direction counter as the nonce, so a
/// device without the secret can neither read the stream nor inject anything into it.
enum WifiCrypto {
    static let magic = Data("SPW1".utf8)
    static let nonceSize = 16
    static let tagSize = 16

    struct Keys {
        let send: SymmetricKey
        let receive: SymmetricKey
    }

    static func newSecret() -> Data { SymmetricKey(size: .bits256).withUnsafeBytes { Data($0) } }

    static func randomNonce() -> Data { SymmetricKey(size: .bits128).withUnsafeBytes { Data($0) } }

    /// Session keys for the Mac (server) side.
    static func serverKeys(secret: Data, clientNonce: Data, serverNonce: Data) -> Keys {
        let salt = clientNonce + serverNonce
        func derive(_ info: String) -> SymmetricKey {
            HKDF<SHA256>.deriveKey(inputKeyMaterial: SymmetricKey(data: secret), salt: salt,
                                   info: Data(info.utf8), outputByteCount: 32)
        }
        return Keys(send: derive("spanly s2c"), receive: derive("spanly c2s"))
    }

    private static func nonce(_ counter: UInt64) -> AES.GCM.Nonce {
        var bytes = [UInt8](repeating: 0, count: 12)
        for i in 0..<8 { bytes[4 + i] = UInt8(truncatingIfNeeded: counter >> (56 - 8 * i)) }
        return try! AES.GCM.Nonce(data: bytes) // 12 bytes is always valid
    }

    /// ciphertext || tag
    static func seal(_ plain: Data, key: SymmetricKey, counter: UInt64) -> Data {
        let box = try! AES.GCM.seal(plain, using: key, nonce: nonce(counter)) // cannot fail for valid keys
        return box.ciphertext + box.tag
    }

    static func open(_ record: Data, key: SymmetricKey, counter: UInt64) throws -> Data {
        guard record.count >= tagSize else { throw CryptoKitError.authenticationFailure }
        let ct = record.prefix(record.count - tagSize)
        let tag = record.suffix(tagSize)
        let box = try AES.GCM.SealedBox(nonce: nonce(counter), ciphertext: ct, tag: tag)
        return try AES.GCM.open(box, using: key)
    }
}
