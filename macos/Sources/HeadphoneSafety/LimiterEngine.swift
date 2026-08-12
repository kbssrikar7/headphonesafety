import AudioToolbox
import CoreAudio
import CoreGraphics
import Foundation

// Bridges BlackHole (captured via a raw HAL input Audio Unit) through Apple's built-in
// peak limiter Audio Unit, into the real headphone device - entirely with manually
// wired Audio Units (a classic pull-chain: output unit's render callback pulls from
// the limiter unit via AudioUnitRender, the limiter's render callback pulls from a
// ring buffer fed by the capture unit).
//
// This replaced an AVAudioEngine-based version that looked completely healthy end to
// end (correct frame counts, correct non-zero sample peaks confirmed at every stage,
// correct device binding confirmed via readback, correct format confirmed via
// readback) but never produced audible output. Isolated the exact same output-device-
// binding technique used here in a bare, non-AVAudioEngine test and confirmed it
// produces real audible sound - the bug was specifically in how AVAudioEngine's
// outputNode wraps a manually reassigned CurrentDevice, not in the audio pipeline
// logic itself. Manual Audio Unit wiring throughout avoids that wrapper entirely.
final class LimiterEngine {
    private static let channelCount = 2
    private static let ringCapacitySeconds = 2.0

    private var captureUnit: AudioUnit?
    private var renderBufferList: UnsafeMutableAudioBufferListPointer?
    private var limiterUnit: AudioUnit?
    private var outputUnit: AudioUnit?

    private var ringBuffers: [RingBuffer] = []
    private var sampleRate: Double = 48000
    private var targetOutputDevice: AudioDeviceID?
    private var savedDefaultInput: AudioDeviceID?

    private(set) var isRunning = false
    var onEngineFailure: (() -> Void)?

    // If the real output device (e.g. Bluetooth headphones) physically disconnects
    // while the output Audio Unit is bound to it, CoreAudio HAL calls on that unit -
    // even a read-only property query like CurrentDevice - can block the calling
    // thread indefinitely waiting for an I/O acknowledgment that never comes, because
    // the device is simply gone. Confirmed live: disconnecting headphones mid-render
    // froze the app's main thread and every diagnostic/revert path with it. This flag,
    // set from a system device-list listener (not the audio thread, not touching the
    // stuck unit), tells every other piece of code to stop calling into outputUnit
    // entirely and instead check via a completely independent, device-list-based path.
    private let deviceRemovalLock = NSLock()
    private var deviceRemovalDetectedStorage = false
    private var deviceRemovalDetected: Bool {
        get { deviceRemovalLock.lock(); defer { deviceRemovalLock.unlock() }; return deviceRemovalDetectedStorage }
        set { deviceRemovalLock.lock(); deviceRemovalDetectedStorage = newValue; deviceRemovalLock.unlock() }
    }
    private var deviceListListenerBlock: AudioObjectPropertyListenerBlock?

    private var watchdogTimer: Timer?
    private var lastRenderedFrames: UInt64 = 0
    private var lastObservedFrames: UInt64 = 0
    private var missedChecks = 0

    private var framesRequestedByOutput: UInt64 = 0
    private var lastRenderStatus: OSStatus = noErr
    private var renderErrorCount: UInt64 = 0
    private var lastCapturePeak: Float = 0
    private var lastSourcePeak: Float = 0
    private var lastOutputPeak: Float = 0
    var onDiagnostics: ((_ framesCaptured: UInt64, _ framesRequestedByOutput: UInt64, _ engineIsRunning: Bool, _ lastRenderStatus: OSStatus, _ renderErrorCount: UInt64, _ actualOutputDevice: AudioDeviceID?, _ targetOutputDevice: AudioDeviceID?, _ capturePeak: Float, _ sourcePeak: Float, _ outputPeak: Float) -> Void)?

    // Capturing from a virtual loopback device like BlackHole reads audio that
    // originated from other apps' output, and recent macOS versions gate that under
    // "Screen & System Audio Recording" - a separate permission from Microphone, with
    // no usage-description string, just an OS prompt.
    private static func ensureScreenRecordingPermissionRequested() {
        CGRequestScreenCaptureAccess()
    }

    func start(blackHoleDevice: AudioDeviceID, realOutputDevice: AudioDeviceID, headroomDb: Float) -> Bool {
        stop()
        Self.ensureScreenRecordingPermissionRequested()

        // Build the graph around BlackHole's own native rate. BlackHole and the real
        // device are frequently clocked differently (e.g. 48kHz vs a Bluetooth
        // headset's 44.1kHz) - tried asking the capture unit to convert on the way in
        // and that reliably failed with kAudioUnitErr_CannotDoInCurrentContext
        // (-10863): AUHAL input-side sample-rate conversion isn't dependable.
        // Output-side conversion is well-supported, so capture and the limiter both
        // stay at BlackHole's native rate, and only the final hop to hardware does
        // any rate conversion.
        sampleRate = Self.readNominalSampleRate(blackHoleDevice)
        let capacity = Int(sampleRate * Self.ringCapacitySeconds)
        ringBuffers = (0..<Self.channelCount).map { _ in RingBuffer(capacity: capacity) }

        // BlackHole ships with its own virtual volume (both scopes), independent of
        // any app's playback volume - found it sitting at ~31% scalar on this
        // machine, silently attenuating everything captured through it well before
        // any of our own gain/limiting logic ever saw the signal. Force it to unity
        // so what we capture is the true, unattenuated signal.
        Self.setUnityVolume(blackHoleDevice)

        savedDefaultInput = CoreAudioUtils.defaultInputDevice()
        if let nonBluetoothInput = CoreAudioUtils.preferredNonBluetoothInputDevice(),
           nonBluetoothInput != savedDefaultInput {
            CoreAudioUtils.setDefaultInputDevice(nonBluetoothInput)
        }

        guard setUpCaptureUnit(device: blackHoleDevice) else {
            tearDownAll()
            return false
        }
        guard setUpLimiterUnit(headroomDb: headroomDb) else {
            tearDownAll()
            return false
        }
        guard setUpOutputUnit(device: realOutputDevice) else {
            tearDownAll()
            return false
        }

        guard let capture = captureUnit, AudioOutputUnitStart(capture) == noErr else {
            tearDownAll()
            return false
        }
        guard let output = outputUnit, AudioOutputUnitStart(output) == noErr else {
            tearDownAll()
            return false
        }

        targetOutputDevice = realOutputDevice
        isRunning = true
        lastRenderedFrames = 0
        lastObservedFrames = 0
        missedChecks = 0
        deviceRemovalDetected = false
        registerDeviceListListener(watching: realOutputDevice)
        startWatchdog()
        return true
    }

    func stop() {
        watchdogTimer?.invalidate()
        watchdogTimer = nil
        unregisterDeviceListListener()
        tearDownAll()
        for buffer in ringBuffers { buffer.reset() }
        isRunning = false
        targetOutputDevice = nil
        deviceRemovalDetected = false
        if let saved = savedDefaultInput {
            CoreAudioUtils.setDefaultInputDevice(saved)
        }
        savedDefaultInput = nil
    }

    // MARK: - Device removal detection

    // Watches the system-wide device list. Deliberately does NOT re-query which
    // devices are present here - a per-device property query (even a plain
    // enumeration, even off the main thread) against a device that's mid-Bluetooth-
    // teardown has been observed to block indefinitely, which would silently defeat
    // this exact listener. Any device-list change at all while the limiter is running
    // is treated as "the device might be gone" and triggers an unconditional revert -
    // a spurious revert on an unrelated device change (e.g. plugging in a USB mic) is
    // cheap, since the limiter just restarts on the next tick if headphones are still
    // there; a hang here is not recoverable at all.
    private func registerDeviceListListener(watching device: AudioDeviceID) {
        var address = AudioObjectPropertyAddress(
            mSelector: kAudioHardwarePropertyDevices,
            mScope: kAudioObjectPropertyScopeGlobal,
            mElement: kAudioObjectPropertyElementMain
        )
        let block: AudioObjectPropertyListenerBlock = { [weak self] _, _ in
            guard let self else { return }
            self.deviceRemovalDetected = true
            DispatchQueue.main.async {
                self.onEngineFailure?()
            }
        }
        deviceListListenerBlock = block
        AudioObjectAddPropertyListenerBlock(AudioObjectID(kAudioObjectSystemObject), &address, DispatchQueue.global(qos: .utility), block)
    }

    private func unregisterDeviceListListener() {
        guard let block = deviceListListenerBlock else { return }
        var address = AudioObjectPropertyAddress(
            mSelector: kAudioHardwarePropertyDevices,
            mScope: kAudioObjectPropertyScopeGlobal,
            mElement: kAudioObjectPropertyElementMain
        )
        AudioObjectRemovePropertyListenerBlock(AudioObjectID(kAudioObjectSystemObject), &address, DispatchQueue.global(qos: .utility), block)
        deviceListListenerBlock = nil
    }

    func updateHeadroom(_ headroomDb: Float) {
        applyHeadroom(headroomDb)
    }

    // MARK: - Capture (BlackHole input)

    private func setUpCaptureUnit(device: AudioDeviceID) -> Bool {
        var desc = AudioComponentDescription(
            componentType: kAudioUnitType_Output,
            componentSubType: kAudioUnitSubType_HALOutput,
            componentManufacturer: kAudioUnitManufacturer_Apple,
            componentFlags: 0,
            componentFlagsMask: 0
        )
        guard let component = AudioComponentFindNext(nil, &desc) else { return false }
        var unitOpt: AudioUnit?
        guard AudioComponentInstanceNew(component, &unitOpt) == noErr, let unit = unitOpt else { return false }

        var enableIO: UInt32 = 1
        var disableIO: UInt32 = 0
        guard AudioUnitSetProperty(unit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Input, 1, &enableIO, UInt32(MemoryLayout<UInt32>.size)) == noErr,
              AudioUnitSetProperty(unit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Output, 0, &disableIO, UInt32(MemoryLayout<UInt32>.size)) == noErr else {
            return false
        }

        var deviceID = device
        guard AudioUnitSetProperty(unit, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0, &deviceID, UInt32(MemoryLayout<AudioDeviceID>.size)) == noErr else {
            return false
        }

        var format = Self.streamFormat(sampleRate: sampleRate)
        guard AudioUnitSetProperty(unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 1, &format, UInt32(MemoryLayout<AudioStreamBasicDescription>.size)) == noErr else {
            return false
        }

        let maxFrames: UInt32 = 4096
        var maxFramesVar = maxFrames
        AudioUnitSetProperty(unit, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0, &maxFramesVar, UInt32(MemoryLayout<UInt32>.size))

        let abl = AudioBufferList.allocate(maximumBuffers: Self.channelCount)
        for i in 0..<Self.channelCount {
            abl[i] = AudioBuffer(mNumberChannels: 1, mDataByteSize: maxFrames * 4, mData: malloc(Int(maxFrames) * 4))
        }
        renderBufferList = abl

        var callbackStruct = AURenderCallbackStruct(
            inputProc: limiterEngineCaptureCallback,
            inputProcRefCon: Unmanaged.passUnretained(self).toOpaque()
        )
        guard AudioUnitSetProperty(unit, kAudioOutputUnitProperty_SetInputCallback, kAudioUnitScope_Global, 0, &callbackStruct, UInt32(MemoryLayout<AURenderCallbackStruct>.size)) == noErr else {
            return false
        }

        guard AudioUnitInitialize(unit) == noErr else { return false }
        captureUnit = unit
        return true
    }

    fileprivate func handleCapture(
        ioActionFlags: UnsafeMutablePointer<AudioUnitRenderActionFlags>,
        inTimeStamp: UnsafePointer<AudioTimeStamp>,
        inBusNumber: UInt32,
        inNumberFrames: UInt32
    ) -> OSStatus {
        guard let unit = captureUnit, let abl = renderBufferList else { return noErr }
        for i in 0..<abl.count {
            abl[i].mDataByteSize = inNumberFrames * 4
        }
        let status = AudioUnitRender(unit, ioActionFlags, inTimeStamp, inBusNumber, inNumberFrames, abl.unsafeMutablePointer)
        guard status == noErr else {
            lastRenderStatus = status
            renderErrorCount &+= 1
            return status
        }

        var peak: Float = 0
        for (channel, buffer) in ringBuffers.enumerated() where channel < abl.count {
            if let data = abl[channel].mData?.assumingMemoryBound(to: Float.self) {
                buffer.write(data, count: Int(inNumberFrames))
                for i in 0..<Int(inNumberFrames) {
                    peak = max(peak, abs(data[i]))
                }
            }
        }
        lastCapturePeak = peak
        lastRenderedFrames &+= UInt64(inNumberFrames)
        return noErr
    }

    // MARK: - Limiter (in-process Audio Unit, pulled by the output unit)

    private func setUpLimiterUnit(headroomDb: Float) -> Bool {
        var desc = AudioComponentDescription(
            componentType: kAudioUnitType_Effect,
            componentSubType: kAudioUnitSubType_PeakLimiter,
            componentManufacturer: kAudioUnitManufacturer_Apple,
            componentFlags: 0,
            componentFlagsMask: 0
        )
        guard let component = AudioComponentFindNext(nil, &desc) else { return false }
        var unitOpt: AudioUnit?
        guard AudioComponentInstanceNew(component, &unitOpt) == noErr, let unit = unitOpt else { return false }

        var format = Self.streamFormat(sampleRate: sampleRate)
        guard AudioUnitSetProperty(unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &format, UInt32(MemoryLayout<AudioStreamBasicDescription>.size)) == noErr,
              AudioUnitSetProperty(unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 0, &format, UInt32(MemoryLayout<AudioStreamBasicDescription>.size)) == noErr else {
            return false
        }

        var callbackStruct = AURenderCallbackStruct(
            inputProc: limiterEngineFillFromRingBuffers,
            inputProcRefCon: Unmanaged.passUnretained(self).toOpaque()
        )
        guard AudioUnitSetProperty(unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &callbackStruct, UInt32(MemoryLayout<AURenderCallbackStruct>.size)) == noErr else {
            return false
        }

        guard AudioUnitInitialize(unit) == noErr else { return false }
        limiterUnit = unit
        applyHeadroom(headroomDb)
        return true
    }

    fileprivate func fillFromRingBuffers(
        ioActionFlags: UnsafeMutablePointer<AudioUnitRenderActionFlags>,
        frameCount: UInt32,
        ioData: UnsafeMutablePointer<AudioBufferList>?
    ) -> OSStatus {
        guard let ioData = ioData else { return noErr }
        framesRequestedByOutput &+= UInt64(frameCount)
        let abl = UnsafeMutableAudioBufferListPointer(ioData)
        var peak: Float = 0
        for (channel, ring) in ringBuffers.enumerated() where channel < abl.count {
            if let ptr = abl[channel].mData?.assumingMemoryBound(to: Float.self) {
                ring.read(into: ptr, count: Int(frameCount))
                for i in 0..<Int(frameCount) {
                    peak = max(peak, abs(ptr[i]))
                }
            }
        }
        lastSourcePeak = peak
        return noErr
    }

    private func applyHeadroom(_ headroomDb: Float) {
        guard let unit = limiterUnit else { return }
        // AULimiter has no explicit "threshold" - PreGain (dB) drives how much signal
        // gets pushed into a limiter with a fixed ceiling, which is what gives us the
        // headroom-below-max behavior consistent with the rest of this app. The
        // limiter's hard ceiling then guarantees no output sample ever exceeds 0dBFS,
        // regardless of what's playing - true peak safety, not just a level clamp.
        AudioUnitSetParameter(unit, kLimiterParam_PreGain, kAudioUnitScope_Global, 0, -headroomDb, 0)
    }

    // MARK: - Output (real headphone device, pulls from the limiter)

    private func setUpOutputUnit(device: AudioDeviceID) -> Bool {
        var desc = AudioComponentDescription(
            componentType: kAudioUnitType_Output,
            componentSubType: kAudioUnitSubType_HALOutput,
            componentManufacturer: kAudioUnitManufacturer_Apple,
            componentFlags: 0,
            componentFlagsMask: 0
        )
        guard let component = AudioComponentFindNext(nil, &desc) else { return false }
        var unitOpt: AudioUnit?
        guard AudioComponentInstanceNew(component, &unitOpt) == noErr, let unit = unitOpt else { return false }

        var enableOutput: UInt32 = 1
        var disableInput: UInt32 = 0
        guard AudioUnitSetProperty(unit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Output, 0, &enableOutput, UInt32(MemoryLayout<UInt32>.size)) == noErr,
              AudioUnitSetProperty(unit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Input, 1, &disableInput, UInt32(MemoryLayout<UInt32>.size)) == noErr else {
            return false
        }

        var deviceID = device
        guard AudioUnitSetProperty(unit, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0, &deviceID, UInt32(MemoryLayout<AudioDeviceID>.size)) == noErr else {
            return false
        }

        var format = Self.streamFormat(sampleRate: sampleRate)
        guard AudioUnitSetProperty(unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &format, UInt32(MemoryLayout<AudioStreamBasicDescription>.size)) == noErr else {
            return false
        }

        var callbackStruct = AURenderCallbackStruct(
            inputProc: limiterEnginePullFromLimiter,
            inputProcRefCon: Unmanaged.passUnretained(self).toOpaque()
        )
        guard AudioUnitSetProperty(unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &callbackStruct, UInt32(MemoryLayout<AURenderCallbackStruct>.size)) == noErr else {
            return false
        }

        guard AudioUnitInitialize(unit) == noErr else { return false }
        outputUnit = unit
        return true
    }

    fileprivate func pullFromLimiter(
        ioActionFlags: UnsafeMutablePointer<AudioUnitRenderActionFlags>,
        inTimeStamp: UnsafePointer<AudioTimeStamp>,
        inBusNumber: UInt32,
        inNumberFrames: UInt32,
        ioData: UnsafeMutablePointer<AudioBufferList>?
    ) -> OSStatus {
        guard let limiter = limiterUnit, let ioData = ioData else { return noErr }
        let status = AudioUnitRender(limiter, ioActionFlags, inTimeStamp, 0, inNumberFrames, ioData)
        if status == noErr {
            var peak: Float = 0
            let abl = UnsafeMutableAudioBufferListPointer(ioData)
            for buffer in abl {
                guard let ptr = buffer.mData?.assumingMemoryBound(to: Float.self) else { continue }
                for i in 0..<Int(inNumberFrames) { peak = max(peak, abs(ptr[i])) }
            }
            lastOutputPeak = peak
        }
        return status
    }

    private func currentOutputDevice() -> AudioDeviceID? {
        // Once device removal is detected, never touch outputUnit again - even this
        // read-only property query can block indefinitely on a unit bound to hardware
        // that's already gone.
        guard !deviceRemovalDetected, let unit = outputUnit else { return nil }
        var deviceID = AudioDeviceID(0)
        var size = UInt32(MemoryLayout<AudioDeviceID>.size)
        let status = AudioUnitGetProperty(unit, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0, &deviceID, &size)
        return status == noErr ? deviceID : nil
    }

    // MARK: - Teardown

    private func tearDownAll() {
        if let unit = captureUnit {
            AudioOutputUnitStop(unit)
            AudioUnitUninitialize(unit)
            AudioComponentInstanceDispose(unit)
        }
        captureUnit = nil
        if let abl = renderBufferList {
            for buffer in abl { free(buffer.mData) }
        }
        renderBufferList = nil

        if let unit = outputUnit {
            if deviceRemovalDetected {
                // The bound device is gone - Stop/Uninitialize/Dispose on this unit
                // have been observed to hang indefinitely waiting for an I/O
                // acknowledgment from hardware that no longer exists. Abandoning the
                // unit (best-effort async cleanup, may never complete) is a one-time
                // resource leak; blocking the caller here is a hang of the entire
                // app's revert path, which is strictly worse and exactly the failure
                // this app's rollback guarantee exists to prevent.
                DispatchQueue.global(qos: .utility).async {
                    AudioOutputUnitStop(unit)
                    AudioUnitUninitialize(unit)
                    AudioComponentInstanceDispose(unit)
                }
            } else {
                AudioOutputUnitStop(unit)
                AudioUnitUninitialize(unit)
                AudioComponentInstanceDispose(unit)
            }
        }
        outputUnit = nil

        if let unit = limiterUnit {
            AudioUnitUninitialize(unit)
            AudioComponentInstanceDispose(unit)
        }
        limiterUnit = nil
    }

    // MARK: - Watchdog

    private func startWatchdog() {
        watchdogTimer = Timer.scheduledTimer(withTimeInterval: 1.5, repeats: true) { [weak self] _ in
            self?.checkLiveness()
        }
    }

    private func checkLiveness() {
        // Defense in depth: the device-list listener already dispatches
        // onEngineFailure directly the moment removal is detected, but if that
        // somehow didn't fire, this catches it on the next 1.5s tick regardless -
        // and never touches outputUnit itself either way (currentOutputDevice()
        // guards on the same flag).
        if deviceRemovalDetected {
            onEngineFailure?()
            return
        }
        onDiagnostics?(lastRenderedFrames, framesRequestedByOutput, isRunning, lastRenderStatus, renderErrorCount, currentOutputDevice(), targetOutputDevice, lastCapturePeak, lastSourcePeak, lastOutputPeak)
        if lastRenderedFrames == lastObservedFrames {
            missedChecks += 1
        } else {
            missedChecks = 0
        }
        lastObservedFrames = lastRenderedFrames

        // Two consecutive quiet windows (~3s) with no captured audio at all is treated
        // as a stall - could be legitimate silence, but erring toward reverting to the
        // real device is the safe choice per the app's rollback guarantees.
        if missedChecks >= 2 {
            onEngineFailure?()
        }
    }

    // MARK: - Helpers

    private static func streamFormat(sampleRate: Double) -> AudioStreamBasicDescription {
        AudioStreamBasicDescription(
            mSampleRate: sampleRate,
            mFormatID: kAudioFormatLinearPCM,
            mFormatFlags: kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked | kAudioFormatFlagIsNonInterleaved,
            mBytesPerPacket: 4,
            mFramesPerPacket: 1,
            mBytesPerFrame: 4,
            mChannelsPerFrame: UInt32(channelCount),
            mBitsPerChannel: 32,
            mReserved: 0
        )
    }

    private static func setUnityVolume(_ device: AudioDeviceID) {
        for scope in [kAudioDevicePropertyScopeOutput, kAudioDevicePropertyScopeInput] {
            var address = AudioObjectPropertyAddress(
                mSelector: kAudioHardwareServiceDeviceProperty_VirtualMainVolume,
                mScope: scope,
                mElement: kAudioObjectPropertyElementMain
            )
            guard AudioObjectHasProperty(device, &address) else { continue }
            var value: Float32 = 1.0
            AudioObjectSetPropertyData(device, &address, 0, nil, UInt32(MemoryLayout<Float32>.size), &value)
        }
    }

    private static func readNominalSampleRate(_ device: AudioDeviceID) -> Double {
        var rate: Float64 = 48000
        var size = UInt32(MemoryLayout<Float64>.size)
        var address = AudioObjectPropertyAddress(
            mSelector: kAudioDevicePropertyNominalSampleRate,
            mScope: kAudioObjectPropertyScopeGlobal,
            mElement: kAudioObjectPropertyElementMain
        )
        AudioObjectGetPropertyData(device, &address, 0, nil, &size, &rate)
        return rate
    }
}

private func limiterEngineCaptureCallback(
    inRefCon: UnsafeMutableRawPointer,
    ioActionFlags: UnsafeMutablePointer<AudioUnitRenderActionFlags>,
    inTimeStamp: UnsafePointer<AudioTimeStamp>,
    inBusNumber: UInt32,
    inNumberFrames: UInt32,
    ioData: UnsafeMutablePointer<AudioBufferList>?
) -> OSStatus {
    let engine = Unmanaged<LimiterEngine>.fromOpaque(inRefCon).takeUnretainedValue()
    return engine.handleCapture(
        ioActionFlags: ioActionFlags,
        inTimeStamp: inTimeStamp,
        inBusNumber: inBusNumber,
        inNumberFrames: inNumberFrames
    )
}

private func limiterEngineFillFromRingBuffers(
    inRefCon: UnsafeMutableRawPointer,
    ioActionFlags: UnsafeMutablePointer<AudioUnitRenderActionFlags>,
    inTimeStamp: UnsafePointer<AudioTimeStamp>,
    inBusNumber: UInt32,
    inNumberFrames: UInt32,
    ioData: UnsafeMutablePointer<AudioBufferList>?
) -> OSStatus {
    let engine = Unmanaged<LimiterEngine>.fromOpaque(inRefCon).takeUnretainedValue()
    return engine.fillFromRingBuffers(ioActionFlags: ioActionFlags, frameCount: inNumberFrames, ioData: ioData)
}

private func limiterEnginePullFromLimiter(
    inRefCon: UnsafeMutableRawPointer,
    ioActionFlags: UnsafeMutablePointer<AudioUnitRenderActionFlags>,
    inTimeStamp: UnsafePointer<AudioTimeStamp>,
    inBusNumber: UInt32,
    inNumberFrames: UInt32,
    ioData: UnsafeMutablePointer<AudioBufferList>?
) -> OSStatus {
    let engine = Unmanaged<LimiterEngine>.fromOpaque(inRefCon).takeUnretainedValue()
    return engine.pullFromLimiter(
        ioActionFlags: ioActionFlags,
        inTimeStamp: inTimeStamp,
        inBusNumber: inBusNumber,
        inNumberFrames: inNumberFrames,
        ioData: ioData
    )
}
