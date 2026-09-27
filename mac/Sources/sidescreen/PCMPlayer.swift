import AVFoundation
import CoreAudio

/// Plays 48 kHz 16-bit PCM from the tablet: its own sound (to the Mac's speakers) or its
/// microphone (into the "SideScreen Microphone" device). Keeps at most ~150 ms queued so it
/// never drifts behind; chunks beyond that are dropped.
final class PCMPlayer {
    private let engine = AVAudioEngine()
    private let node = AVAudioPlayerNode()
    private let format: AVAudioFormat
    private let device: AudioDeviceID?
    private let lock = NSLock()
    private var queuedFrames = 0
    private var lastRestart = Date.distantPast
    private var observer: NSObjectProtocol?

    /// `deviceUID`: play into that device instead of the default output.
    init?(channels: Int, deviceUID: String? = nil) {
        guard let f = AVAudioFormat(commonFormat: .pcmFormatFloat32, sampleRate: 48_000,
                                    channels: AVAudioChannelCount(channels), interleaved: false) else { return nil }
        format = f
        if let uid = deviceUID {
            guard let id = AudioDevices.id(forUID: uid) else { return nil }
            device = id
        } else {
            device = nil
        }
        guard selectDevice() else { return nil } // before connecting, so the graph is built for it
        engine.attach(node)
        engine.connect(node, to: engine.mainMixerNode, format: f)
        guard start() else { return nil }
        // Audio devices coming and going stop the engine; start it again (on the next chunk).
        observer = NotificationCenter.default.addObserver(forName: .AVAudioEngineConfigurationChange,
                                                          object: engine, queue: nil) { [weak self] _ in
            self?.lock.withLock { self?.lastRestart = .distantPast }
        }
    }

    deinit {
        if let observer { NotificationCenter.default.removeObserver(observer) }
    }

    private func selectDevice() -> Bool {
        guard var id = device else { return true }
        guard let unit = engine.outputNode.audioUnit else { return false }
        return AudioUnitSetProperty(unit, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0,
                                    &id, UInt32(MemoryLayout<AudioDeviceID>.size)) == noErr
    }

    private func start() -> Bool {
        do {
            try engine.start()
            return true
        } catch {
            log("audio playback: \(error.localizedDescription)")
            return false
        }
    }

    /// The engine stopped (a device change): drop what was queued and start again, at most once a second.
    private func restart() -> Bool {
        let due = lock.withLock {
            guard Date().timeIntervalSince(lastRestart) > 1 else { return false }
            lastRestart = Date()
            return true
        }
        guard due else { return false }
        log("audio playback stopped (device change); restarting")
        node.stop()
        lock.withLock { queuedFrames = 0 }
        return selectDevice() && start()
    }

    func play(_ pcm: Data) {
        guard engine.isRunning || restart() else { return }
        let channels = Int(format.channelCount)
        let frames = pcm.count / (2 * channels)
        guard frames > 0, lock.withLock({ queuedFrames < 7_200 }), // 150 ms
              let buf = AVAudioPCMBuffer(pcmFormat: format, frameCapacity: AVAudioFrameCount(frames)),
              let planes = buf.floatChannelData else { return }
        buf.frameLength = AVAudioFrameCount(frames)
        pcm.withUnsafeBytes { raw in
            let s = raw.bindMemory(to: Int16.self)
            for f in 0..<frames {
                for c in 0..<channels { planes[c][f] = Float(Int16(littleEndian: s[f * channels + c])) / 32768 }
            }
        }
        lock.withLock { queuedFrames += frames }
        node.scheduleBuffer(buf) { [weak self] in self?.lock.withLock { self?.queuedFrames -= frames } }
        // Started only once there is audio: a node started empty stays silent on the loopback device.
        if !node.isPlaying { node.play() }
    }

    func stop() {
        node.stop()
        engine.stop()
    }
}

enum AudioDevices {
    static func id(forUID uid: String) -> AudioDeviceID? {
        var cfUID = uid as CFString
        var device = AudioDeviceID(kAudioObjectUnknown)
        var size = UInt32(MemoryLayout<AudioDeviceID>.size)
        var address = AudioObjectPropertyAddress(
            mSelector: kAudioHardwarePropertyTranslateUIDToDevice,
            mScope: kAudioObjectPropertyScopeGlobal,
            mElement: kAudioObjectPropertyElementMain)
        let status = withUnsafeMutablePointer(to: &cfUID) { uidPtr in
            AudioObjectGetPropertyData(AudioObjectID(kAudioObjectSystemObject), &address,
                                       UInt32(MemoryLayout<CFString>.size), uidPtr, &size, &device)
        }
        return status == noErr && device != kAudioObjectUnknown ? device : nil
    }
}
