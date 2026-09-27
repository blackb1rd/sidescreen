import CryptoKit
import Foundation
import Testing
@testable import sidescreen

/// The same vectors are checked on Android (SecureChannelTest), so both sides must agree.
@Suite struct WifiCryptoTests {
    let secret = Data((1...32).map { UInt8($0) })
    let clientNonce = Data((0..<16).map { UInt8(0xA0 + $0) })
    let serverNonce = Data((0..<16).map { UInt8(0xB0 + $0) })

    func hex(_ k: SymmetricKey) -> String { k.withUnsafeBytes { $0.map { String(format: "%02x", $0) }.joined() } }

    @Test func derivesPerDirectionKeys() {
        let keys = WifiCrypto.serverKeys(secret: secret, clientNonce: clientNonce, serverNonce: serverNonce)
        #expect(hex(keys.receive) == "dad5e4f21f47255de3fbe0fc1523607eea3e8b80ef8044f884b03fc116f02000") // c2s
        #expect(hex(keys.send) == "e44762191fb04c1f1075dd4b6a18c8250bf19da566626b76ad1c8a1e1cd366c1") // s2c
    }

    @Test func sealMatchesTheAndroidVector() {
        let keys = WifiCrypto.serverKeys(secret: secret, clientNonce: clientNonce, serverNonce: serverNonce)
        let sealed = WifiCrypto.seal(Data("hello tablet".utf8), key: keys.send, counter: 5)
        #expect(sealed.map { String(format: "%02x", $0) }.joined() == "82022662e775c2e0a496d1761b53c7ec67538e98813b69a8d2e73c6f")
    }

    @Test func roundTripsAndRejectsTampering() throws {
        let key = SymmetricKey(size: .bits256)
        var sealed = WifiCrypto.seal(Data("frame".utf8), key: key, counter: 9)
        #expect(try WifiCrypto.open(sealed, key: key, counter: 9) == Data("frame".utf8))
        #expect(throws: (any Error).self) { try WifiCrypto.open(sealed, key: key, counter: 10) } // wrong counter
        sealed[sealed.startIndex] ^= 1
        #expect(throws: (any Error).self) { try WifiCrypto.open(sealed, key: key, counter: 9) }
    }
}
