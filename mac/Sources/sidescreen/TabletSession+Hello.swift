import AppKit

/// A tablet announcing itself (HELLO): make or reshape its display, or just resume the stream.
extension TabletSession {
    func hello(_ hello: Hello) {
        lastHello = hello
        teardownWork?.cancel()
        teardownWork = nil
        let hevc = hello.caps & 1 != 0
        if hevc != tabletHEVC {
            tabletHEVC = hevc
            if encoder != nil && encoder?.codec != wantedCodec {
                size = (0, 0) // force a SIZE (with the new codec) once the pipeline restarts
                scheduleRestart(after: 0.2)
                return
            }
        }
        if c.opts.displayName != nil || c.mirroring {
            virtual = nil
            if size.w > 0 && activeBitrate == bitrateMbps {
                startStream()
            } else {
                size = (0, 0)
                scheduleRestart(after: 0.1)
            }
            return
        }
        let (tw, th) = (hello.w, hello.h) // as the tablet is held: landscape or portrait
        if let v = virtual, v.hiDPI == wantHiDPI, (v.tabletW, v.tabletH) == (th, tw) {
            // The tablet turned: reshape the same display so its windows stay on it.
            if v.resize(tabletW: tw, tabletH: th) {
                log("\(name) rotated: display is now \(tw)x\(th)")
                size = (0, 0)
                DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) { [weak self] in
                    guard let self, self.virtual === v else { return }
                    v.selectMode()
                    self.scheduleRestart(after: 0.3)
                }
                return
            }
        }
        if let v = virtual, v.tabletW == tw, v.tabletH == th, v.hiDPI == wantHiDPI {
            // A repeated HELLO while the display is still being set up: the pipeline
            // start will send SIZE and a keyframe when it's ready.
            guard size.w > 0 else { return }
            if activeBitrate != bitrateMbps {
                // New link (e.g. USB -> Wi-Fi): retune the running encoder instead of restarting.
                activeBitrate = bitrateMbps
                currentBitrate = bitrateMbps
                encoder?.setBitrate(Int(bitrateMbps * 1_000_000))
            }
            startStream()
            return
        }
        virtual = nil
        size = (0, 0)
        guard let v = VirtualScreen(tabletW: tw, tabletH: th, dpi: hello.dpi, hiDPI: wantHiDPI, serial: serial) else {
            // Usually the previous display (same serial) hasn't finished going away yet.
            log("could not create virtual display; retrying")
            DispatchQueue.main.asyncAfter(deadline: .now() + 1) { [weak self] in
                guard let self, self.virtual == nil, self.link.isConnected, let h = self.lastHello else { return }
                self.hello(h)
            }
            return
        }
        virtual = v
        log("created virtual display \(v.displayID) for \(name), \(tw)x\(th) (\(wantHiDPI ? "HiDPI" : "standard"), over \(onUsb ? "USB accessory" : onWifi ? "Wi-Fi" : "adb"))")
        // macOS needs a moment to publish the new display's modes and geometry.
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) { [weak self] in
            guard let self, self.virtual === v else { return }
            v.selectMode()
            v.place(self.c.position, others: self.c.sessions.compactMap { $0.virtual?.displayID })
            self.scheduleRestart(after: 0.5)
        }
    }

    /// Make the display again (e.g. Retina switched on), keeping the tablet connected.
    func recreateDisplay() {
        guard let hello = lastHello, link.isConnected else { return }
        Task {
            await stop()
            // Give macOS a moment to remove the old display before making the new one.
            try? await Task.sleep(for: .milliseconds(500))
            await MainActor.run { self.hello(hello) }
        }
    }

    /// Keep the display briefly across reconnects so windows don't jump around; then let go.
    func scheduleTeardown() {
        teardownWork?.cancel()
        let work = DispatchWorkItem { [weak self] in
            guard let self, !self.link.isConnected else { return }
            Task {
                await self.stop()
                await MainActor.run {
                    log("\(self.name) gone; stopped streaming")
                    self.c.remove(self)
                }
            }
        }
        teardownWork = work
        DispatchQueue.main.asyncAfter(deadline: .now() + c.opts.lingerSeconds, execute: work)
    }
}
