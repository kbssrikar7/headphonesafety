import CoreAudio
import Foundation

final class AudioMonitor {
    private var timer: Timer?
    private let pollInterval: TimeInterval = 0.4

    private var cachedDeviceID: AudioDeviceID?
    private var cachedStrategy: CoreAudioUtils.VolumeControlStrategy?

    private let limiterEngine = LimiterEngine()
    private enum LimiterState {
        case idle
        // blackhole/fallbackDevice are captured once, while everything is healthy, at
        // the moment the limiter starts - every check made while limiting reuses these
        // instead of re-querying/enumerating devices, because that enumeration
        // (CoreAudioUtils.allOutputDevices(), BlackHoleDetector.findDevice()) touches
        // every device including one that may be mid-disconnect, and a per-device
        // property query against a device in that state has been observed to block
        // indefinitely - which previously froze this exact polling loop and, with it,
        // the only code path that could revert output back off BlackHole.
        case limiting(realDevice: AudioDeviceID, blackhole: AudioDeviceID, fallbackDevice: AudioDeviceID)
    }
    private var limiterState: LimiterState = .idle

    struct LevelInfo {
        let percent: Int?
        let currentDb: Float32?
        let minDb: Float32?
        let maxDb: Float32?
        let capDb: Float32?
    }

    // deviceName, kind, level (nil when limiter mode makes a dB-clamp reading not
    // applicable), isLimiterActive
    var onStateChange: ((_ deviceName: String, _ kind: OutputKind, _ level: LevelInfo?, _ isLimiterActive: Bool) -> Void)?

    init() {
        limiterEngine.onEngineFailure = { [weak self] in
            self?.revertLimiter()
        }
        limiterEngine.onDiagnostics = { captured, requested, engineRunning, lastStatus, errorCount, actualDevice, targetDevice, capturePeak, sourcePeak, outputPeak in
            let line = "[\(Date())] DIAG captured=\(captured) requestedByOutput=\(requested) engineRunning=\(engineRunning) lastRenderStatus=\(lastStatus) renderErrorCount=\(errorCount) actualOutputDevice=\(String(describing: actualDevice)) targetOutputDevice=\(String(describing: targetDevice)) capturePeak=\(capturePeak) sourcePeak=\(sourcePeak) outputPeak=\(outputPeak)\n"
            if let data = line.data(using: .utf8) {
                if let h = FileHandle(forWritingAtPath: "/tmp/hs-limiter-diag.log") {
                    h.seekToEndOfFile(); h.write(data); h.closeFile()
                } else {
                    try? data.write(to: URL(fileURLWithPath: "/tmp/hs-limiter-diag.log"))
                }
            }
        }
    }

    func start() {
        // Covers "app was killed uncleanly last session while BlackHole was the
        // default output" - never start a session assuming a leftover state is fine.
        Self.revertStrayBlackHoleDefaultOutput()

        timer = Timer.scheduledTimer(withTimeInterval: pollInterval, repeats: true) { [weak self] _ in
            self?.tick()
        }
        timer?.tolerance = 0.1
        tick()
    }

    func stop() {
        timer?.invalidate()
        timer = nil
        revertLimiter()
    }

    static func revertStrayBlackHoleDefaultOutput() {
        guard let current = CoreAudioUtils.defaultOutputDevice(),
              let blackhole = BlackHoleDetector.findDevice(),
              current == blackhole else { return }
        let fallback = CoreAudioUtils.systemOutputDevice() ?? current
        CoreAudioUtils.setDefaultOutputDevice(fallback)
    }

    private func strategy(for device: AudioDeviceID) -> CoreAudioUtils.VolumeControlStrategy {
        if cachedDeviceID == device, let strategy = cachedStrategy {
            return strategy
        }
        let resolved = CoreAudioUtils.resolveControlStrategy(for: device)
        cachedDeviceID = device
        cachedStrategy = resolved
        return resolved
    }

    private static func heartbeat(_ label: String) {
        let line = "[\(Date())] HEARTBEAT \(label)\n"
        if let data = line.data(using: .utf8) {
            if let h = FileHandle(forWritingAtPath: "/tmp/hs-heartbeat.log") {
                h.seekToEndOfFile(); h.write(data); h.closeFile()
            } else {
                try? data.write(to: URL(fileURLWithPath: "/tmp/hs-heartbeat.log"))
            }
        }
    }

    private func tick() {
        Self.heartbeat("tick start, state=\(limiterState)")
        switch limiterState {
        case .limiting(let realDevice, let blackhole, let fallbackDevice):
            tickLimiting(realDevice: realDevice, blackhole: blackhole, fallbackDevice: fallbackDevice)
        case .idle:
            tickIdle()
        }
        Self.heartbeat("tick end")
    }

    private func tickIdle() {
        guard let device = CoreAudioUtils.defaultOutputDevice() else { return }

        // Self-healing safety net: if we're idle (not intentionally running the
        // limiter) but default output is still BlackHole, something's wrong - most
        // likely a revert attempt that landed in a brief OS transition window right
        // after a device disconnect and silently no-op'd (confirmed live: the exact
        // same set-default-output call that no-ops in that window succeeds a moment
        // later). Rather than trust any single revert attempt, keep retrying a safe
        // fallback on every 0.4s tick until it actually takes effect - this is what
        // makes the rollback guarantee hold even through timing windows that can't be
        // reliably avoided up front.
        if let blackhole = BlackHoleDetector.findDevice(), device == blackhole {
            if let fallback = Self.safeFallbackOutputDevice() {
                Self.heartbeat("tickIdle: stray BlackHole default output, retrying fallback")
                CoreAudioUtils.setDefaultOutputDevice(fallback)
            }
            return
        }

        let kind = CoreAudioUtils.classify(device)
        let name = CoreAudioUtils.deviceName(device)
        let isHeadphoneLike = (kind == .builtInHeadphones || kind == .bluetooth)

        if isHeadphoneLike, Settings.shared.limiterEnabled,
           let blackhole = BlackHoleDetector.findDevice(), blackhole != device {
            let fallbackDevice = Self.safeFallbackOutputDevice() ?? device
            if attemptStartLimiter(realDevice: device, blackhole: blackhole) {
                limiterState = .limiting(realDevice: device, blackhole: blackhole, fallbackDevice: fallbackDevice)
                onStateChange?(name, kind, nil, true)
                return
            }
            // Fell through: limiter failed to start, fall back to the dB clamp below.
        }

        let percent = CoreAudioUtils.getVolume(device).map { Int(($0 * 100).rounded()) }
        let level = readAndEnforceClamp(
            device: device,
            strategy: strategy(for: device),
            isHeadphoneLike: isHeadphoneLike,
            percent: percent
        )
        onStateChange?(name, kind, level, false)
    }

    private func tickLimiting(realDevice: AudioDeviceID, blackhole: AudioDeviceID, fallbackDevice: AudioDeviceID) {
        // Deliberately a single system-object property read, not a per-device
        // enumeration - see the note on LimiterState.limiting. Real device
        // disconnection is caught separately, by LimiterEngine's own device-list
        // listener (which triggers onEngineFailure -> revertLimiter below), not by
        // polling here.
        let currentDefault = CoreAudioUtils.defaultOutputDevice()
        let stillOnBlackHole = (currentDefault == blackhole)
        let stillEnabled = Settings.shared.limiterEnabled

        guard stillOnBlackHole, stillEnabled, limiterEngine.isRunning else {
            revertLimiter()
            tickIdle()
            return
        }

        limiterEngine.updateHeadroom(Float(Settings.shared.capHeadroomDb))
        let name = CoreAudioUtils.deviceName(realDevice)
        let kind = CoreAudioUtils.classify(realDevice)
        onStateChange?(name, kind, nil, true)
    }

    // Picks a device that can never be hot-unplugged (built-in speakers), captured
    // once while the system is in a known-healthy state, to use as the guaranteed-
    // safe revert target if the real device turns out to be gone by the time a
    // revert actually happens.
    private static func safeFallbackOutputDevice() -> AudioDeviceID? {
        CoreAudioUtils.allOutputDevices().first { CoreAudioUtils.classify($0) == .builtInSpeaker }
            ?? CoreAudioUtils.systemOutputDevice()
    }

    private func attemptStartLimiter(realDevice: AudioDeviceID, blackhole: AudioDeviceID) -> Bool {
        // Order matters: BlackHole must already be the active default output device
        // before the capture Audio Unit attaches and starts, or its render cycle can
        // lock onto a "nothing is routed here yet" timeline and never pick up audio
        // that starts flowing afterward - captured a real case of this where a
        // standalone test with the order reversed captured live audio fine, while
        // this app's original start-then-switch order captured pure silence
        // indefinitely despite every other diagnostic looking perfectly healthy.
        guard CoreAudioUtils.setDefaultOutputDevice(blackhole) else { return false }

        guard limiterEngine.start(
            blackHoleDevice: blackhole,
            realOutputDevice: realDevice,
            headroomDb: Float(Settings.shared.capHeadroomDb)
        ) else {
            // See the note in revertLimiter(): a set-default-output call reporting
            // success does not mean it actually took effect if the target device is
            // gone, so verify via readback before trusting we're off BlackHole.
            CoreAudioUtils.setDefaultOutputDevice(realDevice)
            if CoreAudioUtils.defaultOutputDevice() != realDevice, let fallback = Self.safeFallbackOutputDevice() {
                CoreAudioUtils.setDefaultOutputDevice(fallback)
            }
            return false
        }
        return true
    }

    private func revertLimiter() {
        Self.heartbeat("revertLimiter start")
        guard case .limiting(let realDevice, _, let fallbackDevice) = limiterState else {
            limiterEngine.stop()
            Self.heartbeat("revertLimiter end (was idle)")
            return
        }
        // Route the system's actual audio output away from BlackHole FIRST, before
        // touching the engine at all. This is a single AudioObjectSetPropertyData call
        // on the system object - confirmed (via an external process, live, while the
        // app's own output Audio Unit was wedged on a disconnected device) that this
        // specific call does not hang even when everything downstream is stuck. Tries
        // the real device first (the common case: a normal toggle-off with the
        // headphones still connected), falling back to a device that's physically
        // guaranteed to still exist if that fails.
        //
        // Critically, "falls back if that fails" cannot mean "if the call returns an
        // error" - confirmed live that AudioObjectSetPropertyData for this property
        // returns noErr and silently no-ops even when targeting a device ID that no
        // longer exists (e.g. just-disconnected Bluetooth headphones). Trusting that
        // return value here previously meant a disconnect always "succeeded" at
        // reverting while leaving output stuck on BlackHole. Reading back the actual
        // resulting device is the only reliable way to know whether it worked.
        CoreAudioUtils.setDefaultOutputDevice(realDevice)
        if CoreAudioUtils.defaultOutputDevice() != realDevice {
            CoreAudioUtils.setDefaultOutputDevice(fallbackDevice)
        }
        limiterState = .idle
        Self.heartbeat("revertLimiter: routing done, stopping engine")
        limiterEngine.stop()
        Self.heartbeat("revertLimiter end")
    }

    // MARK: - dB-clamp fallback (used whenever the real-time limiter isn't active)

    private func readAndEnforceClamp(
        device: AudioDeviceID,
        strategy: CoreAudioUtils.VolumeControlStrategy,
        isHeadphoneLike: Bool,
        percent: Int?
    ) -> LevelInfo? {
        switch strategy {
        case .nativeDecibels(let element, let range):
            guard let currentDb = CoreAudioUtils.getVolumeDecibels(device, element: element),
                  Self.isPlausible(currentDb, in: range) else {
                // Device property reads can transiently return a "success" status with
                // garbage data right around a default-output-device change - skip
                // enforcement for this tick rather than act on a bogus value; it
                // self-corrects on the next 0.4s tick.
                return LevelInfo(percent: percent, currentDb: nil, minDb: nil, maxDb: nil, capDb: nil)
            }
            return applyCap(currentDb: currentDb, range: range, isHeadphoneLike: isHeadphoneLike, percent: percent) { target in
                CoreAudioUtils.setVolumeDecibels(device, element: element, target)
            }

        case .scalarTranslation(let range):
            guard let scalar = CoreAudioUtils.getVolume(device),
                  let currentDb = CoreAudioUtils.scalarToDecibels(device, scalar: scalar),
                  Self.isPlausible(currentDb, in: range) else {
                return LevelInfo(percent: percent, currentDb: nil, minDb: nil, maxDb: nil, capDb: nil)
            }
            return applyCap(currentDb: currentDb, range: range, isHeadphoneLike: isHeadphoneLike, percent: percent) { target in
                if let targetScalar = CoreAudioUtils.decibelsToScalar(device, decibels: target) {
                    CoreAudioUtils.setVolume(device, targetScalar)
                }
            }

        case .scalarOnly:
            if isHeadphoneLike, Settings.shared.capEnabled, let scalar = CoreAudioUtils.getVolume(device) {
                let fallbackCapScalar: Float32 = 0.6
                if scalar > fallbackCapScalar {
                    CoreAudioUtils.setVolume(device, fallbackCapScalar)
                }
            }
            return LevelInfo(percent: percent, currentDb: nil, minDb: nil, maxDb: nil, capDb: nil)
        }
    }

    private static func isPlausible(_ db: Float32, in range: ClosedRange<Float32>) -> Bool {
        guard db.isFinite else { return false }
        let margin: Float32 = 10
        return db >= range.lowerBound - margin && db <= range.upperBound + margin
    }

    private func applyCap(
        currentDb: Float32,
        range: ClosedRange<Float32>,
        isHeadphoneLike: Bool,
        percent: Int?,
        apply: (Float32) -> Void
    ) -> LevelInfo {
        var displayedDb = currentDb
        var capDb: Float32?

        if isHeadphoneLike, Settings.shared.capEnabled {
            let headroom = Float32(Settings.shared.capHeadroomDb)
            let cap = max(range.lowerBound, range.upperBound - headroom)
            capDb = cap
            if currentDb > cap {
                apply(cap)
                displayedDb = cap
            }
        }

        return LevelInfo(
            percent: percent,
            currentDb: displayedDb,
            minDb: range.lowerBound,
            maxDb: range.upperBound,
            capDb: capDb
        )
    }
}
