import CryptoKit
import Foundation
import Testing
@testable import spanly

/// The same vectors are checked on Android (SecureChannelTest), so both sides must agree.
@Suite struct WifiCryptoTests {
    let secret = Data((1...32).map { UInt8($0) })
    let clientNonce = Data((0..<16).map { UInt8(0xA0 + $0) })
    let serverNonce = Data((0..<16).map { UInt8(0xB0 + $0) })

    func hex(_ k: SymmetricKey) -> String { k.withUnsafeBytes { $0.map { String(format: "%02x", $0) }.joined() } }

    @Test func derivesPerDirectionKeys() {
        let keys = WifiCrypto.serverKeys(secret: secret, clientNonce: clientNonce, serverNonce: serverNonce)
        #expect(hex(keys.receive) == "7b00117bc521fd6b60ab168938a85eb36ed13daf2412cf3e81de7430c27ebf00") // c2s
        #expect(hex(keys.send) == "870ed19deb24707f0404ce34e8bb337eaeca4c7422884fe35faa23d1b05cbb57") // s2c
    }

    @Test func sealMatchesTheAndroidVector() {
        let keys = WifiCrypto.serverKeys(secret: secret, clientNonce: clientNonce, serverNonce: serverNonce)
        let sealed = WifiCrypto.seal(Data("hello tablet".utf8), key: keys.send, counter: 5)
        #expect(sealed.map { String(format: "%02x", $0) }.joined() == "0abf9ce6e9fcb34d91cbf7019062ffe0b45bd93794ff6d3b1b1b9eb8")
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
