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
        case limiting(realDevice: AudioDeviceID)
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

    private func tick() {
        switch limiterState {
        case .limiting(let realDevice):
            tickLimiting(realDevice: realDevice)
        case .idle:
            tickIdle()
        }
    }

    private func tickIdle() {
        guard let device = CoreAudioUtils.defaultOutputDevice() else { return }
        let kind = CoreAudioUtils.classify(device)
        let name = CoreAudioUtils.deviceName(device)
        let isHeadphoneLike = (kind == .builtInHeadphones || kind == .bluetooth)

        if isHeadphoneLike, Settings.shared.limiterEnabled,
           let blackhole = BlackHoleDetector.findDevice(), blackhole != device {
            if attemptStartLimiter(realDevice: device, blackhole: blackhole) {
                limiterState = .limiting(realDevice: device)
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

    private func tickLimiting(realDevice: AudioDeviceID) {
        let currentDefault = CoreAudioUtils.defaultOutputDevice()
        let blackhole = BlackHoleDetector.findDevice()
        let stillOnBlackHole = (blackhole != nil && currentDefault == blackhole)
        let stillConnected = CoreAudioUtils.allOutputDevices().contains(realDevice)
        let stillEnabled = Settings.shared.limiterEnabled

        guard stillOnBlackHole, stillConnected, stillEnabled, limiterEngine.isRunning else {
            revertLimiter()
            tickIdle()
            return
        }

        limiterEngine.updateHeadroom(Float(Settings.shared.capHeadroomDb))
        let name = CoreAudioUtils.deviceName(realDevice)
        let kind = CoreAudioUtils.classify(realDevice)
        onStateChange?(name, kind, nil, true)
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
            CoreAudioUtils.setDefaultOutputDevice(realDevice)
            return false
        }
        return true
    }

    private func revertLimiter() {
        guard case .limiting(let realDevice) = limiterState else {
            limiterEngine.stop()
            return
        }
        limiterEngine.stop()
        if !CoreAudioUtils.setDefaultOutputDevice(realDevice) {
            if let fallback = CoreAudioUtils.systemOutputDevice() {
                CoreAudioUtils.setDefaultOutputDevice(fallback)
            }
        }
        limiterState = .idle
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
