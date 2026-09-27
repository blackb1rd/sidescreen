import CoreAudio
import Foundation

/// "Tablet Only" sound: mute the Mac's own output while its sound plays on the tablet.
/// ScreenCaptureKit captures app audio before the output device, so muting doesn't silence
/// the tablet. The previous state is restored afterwards (also after a crash, on next launch).
enum MacSpeakers {
    private static let mutedKey = "mutedMacSpeakers"

    private static func outputDevice() -> AudioObjectID? {
        var id = AudioObjectID(kAudioObjectUnknown)
        var size = UInt32(MemoryLayout<AudioObjectID>.size)
        var address = AudioObjectPropertyAddress(
            mSelector: kAudioHardwarePropertyDefaultOutputDevice,
            mScope: kAudioObjectPropertyScopeGlobal,
            mElement: kAudioObjectPropertyElementMain)
        let status = AudioObjectGetPropertyData(AudioObjectID(kAudioObjectSystemObject), &address, 0, nil, &size, &id)
        return status == noErr && id != kAudioObjectUnknown ? id : nil
    }

    private static var muteAddress = AudioObjectPropertyAddress(
        mSelector: kAudioDevicePropertyMute,
        mScope: kAudioDevicePropertyScopeOutput,
        mElement: kAudioObjectPropertyElementMain)

    private static func setMuted(_ muted: Bool) {
        guard let device = outputDevice() else { return }
        var value: UInt32 = muted ? 1 : 0
        AudioObjectSetPropertyData(device, &muteAddress, 0, nil, UInt32(MemoryLayout<UInt32>.size), &value)
    }

    private static func isMuted() -> Bool {
        guard let device = outputDevice() else { return false }
        var value: UInt32 = 0
        var size = UInt32(MemoryLayout<UInt32>.size)
        return AudioObjectGetPropertyData(device, &muteAddress, 0, nil, &size, &value) == noErr && value != 0
    }

    /// Mute the Mac, remembering that we did (only if it wasn't muted already).
    static func mute() {
        guard !UserDefaults.standard.bool(forKey: mutedKey), !isMuted() else { return }
        UserDefaults.standard.set(true, forKey: mutedKey)
        setMuted(true)
        log("Mac speakers muted: sound plays on the tablet only")
    }

    /// Undo `mute()` if we muted.
    static func restore() {
        guard UserDefaults.standard.bool(forKey: mutedKey) else { return }
        UserDefaults.standard.set(false, forKey: mutedKey)
        setMuted(false)
        log("Mac speakers restored")
    }
}
