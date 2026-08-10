import AudioToolbox
import CoreAudio
import Foundation

enum OutputKind {
    case builtInSpeaker
    case builtInHeadphones
    case bluetooth
    case usbOrOther
}

func fourCC(_ s: String) -> UInt32 {
    var result: UInt32 = 0
    for byte in s.utf8 { result = (result << 8) + UInt32(byte) }
    return result
}

private let kHeadphoneDataSource = fourCC("hdpn")

struct CoreAudioDevice {
    let id: AudioDeviceID
}

enum CoreAudioUtils {

    static func defaultOutputDevice() -> AudioDeviceID? {
        var deviceID = AudioDeviceID(0)
        var size = UInt32(MemoryLayout<AudioDeviceID>.size)
        var address = AudioObjectPropertyAddress(
            mSelector: kAudioHardwarePropertyDefaultOutputDevice,
            mScope: kAudioObjectPropertyScopeGlobal,
            mElement: kAudioObjectPropertyElementMain
        )
        let status = AudioObjectGetPropertyData(
            AudioObjectID(kAudioObjectSystemObject), &address, 0, nil, &size, &deviceID
        )
        guard status == noErr, deviceID != 0 else { return nil }
        return deviceID
    }

    @discardableResult
    static func setDefaultOutputDevice(_ device: AudioDeviceID) -> Bool {
        var address = AudioObjectPropertyAddress(
            mSelector: kAudioHardwarePropertyDefaultOutputDevice,
            mScope: kAudioObjectPropertyScopeGlobal,
            mElement: kAudioObjectPropertyElementMain
        )
        var value = device
        let size = UInt32(MemoryLayout<AudioDeviceID>.size)
        let status = AudioObjectSetPropertyData(
            AudioObjectID(kAudioObjectSystemObject), &address, 0, nil, size, &value
        )
        return status == noErr
    }

    // The OS's own "default system output" (used for alerts/system sounds) - almost
    // always resolves to a real, always-present device (built-in speakers). Used as a
    // last-resort revert target if the specific device we meant to revert to has since
    // been unplugged.
    static func systemOutputDevice() -> AudioDeviceID? {
        var deviceID = AudioDeviceID(0)
        var size = UInt32(MemoryLayout<AudioDeviceID>.size)
        var address = AudioObjectPropertyAddress(
            mSelector: kAudioHardwarePropertyDefaultSystemOutputDevice,
            mScope: kAudioObjectPropertyScopeGlobal,
            mElement: kAudioObjectPropertyElementMain
        )
        let status = AudioObjectGetPropertyData(
            AudioObjectID(kAudioObjectSystemObject), &address, 0, nil, &size, &deviceID
        )
        guard status == noErr, deviceID != 0 else { return nil }
        return deviceID
    }

    static func defaultInputDevice() -> AudioDeviceID? {
        var deviceID = AudioDeviceID(0)
        var size = UInt32(MemoryLayout<AudioDeviceID>.size)
        var address = AudioObjectPropertyAddress(
            mSelector: kAudioHardwarePropertyDefaultInputDevice,
            mScope: kAudioObjectPropertyScopeGlobal,
            mElement: kAudioObjectPropertyElementMain
        )
        let status = AudioObjectGetPropertyData(
            AudioObjectID(kAudioObjectSystemObject), &address, 0, nil, &size, &deviceID
        )
        guard status == noErr, deviceID != 0 else { return nil }
        return deviceID
    }

    @discardableResult
    static func setDefaultInputDevice(_ device: AudioDeviceID) -> Bool {
        var address = AudioObjectPropertyAddress(
            mSelector: kAudioHardwarePropertyDefaultInputDevice,
            mScope: kAudioObjectPropertyScopeGlobal,
            mElement: kAudioObjectPropertyElementMain
        )
        var value = device
        let size = UInt32(MemoryLayout<AudioDeviceID>.size)
        let status = AudioObjectSetPropertyData(
            AudioObjectID(kAudioObjectSystemObject), &address, 0, nil, size, &value
        )
        return status == noErr
    }

    static func allInputDevices() -> [AudioDeviceID] {
        var address = AudioObjectPropertyAddress(
            mSelector: kAudioHardwarePropertyDevices,
            mScope: kAudioObjectPropertyScopeGlobal,
            mElement: kAudioObjectPropertyElementMain
        )
        var size: UInt32 = 0
        guard AudioObjectGetPropertyDataSize(AudioObjectID(kAudioObjectSystemObject), &address, 0, nil, &size) == noErr else {
            return []
        }
        let count = Int(size) / MemoryLayout<AudioDeviceID>.size
        var devices = [AudioDeviceID](repeating: 0, count: count)
        guard AudioObjectGetPropertyData(AudioObjectID(kAudioObjectSystemObject), &address, 0, nil, &size, &devices) == noErr else {
            return []
        }
        return devices.filter { hasInputStreams($0) }
    }

    private static func hasInputStreams(_ device: AudioDeviceID) -> Bool {
        var address = AudioObjectPropertyAddress(
            mSelector: kAudioDevicePropertyStreams,
            mScope: kAudioDevicePropertyScopeInput,
            mElement: kAudioObjectPropertyElementMain
        )
        var size: UInt32 = 0
        guard AudioObjectGetPropertyDataSize(device, &address, 0, nil, &size) == noErr else { return false }
        return size > 0
    }

    static func transportTypeGlobal(_ device: AudioDeviceID) -> UInt32 {
        var transport: UInt32 = 0
        var size = UInt32(MemoryLayout<UInt32>.size)
        var address = AudioObjectPropertyAddress(
            mSelector: kAudioDevicePropertyTransportType,
            mScope: kAudioObjectPropertyScopeGlobal,
            mElement: kAudioObjectPropertyElementMain
        )
        AudioObjectGetPropertyData(device, &address, 0, nil, &size, &transport)
        return transport
    }

    // Picks a non-Bluetooth input device to use as the system default input while the
    // real-time limiter is capturing from BlackHole. Leaving a Bluetooth headset as
    // the default input risks the OS renegotiating its profile from A2DP (stereo,
    // output-only) down to HFP (mono, bidirectional) the moment any process opens an
    // input stream - which silently kills the very A2DP stream the limiter plays
    // through. Confirmed via a standalone test: switching default input away from the
    // Bluetooth headset first turned a previously-silent capture immediately audible.
    static func preferredNonBluetoothInputDevice() -> AudioDeviceID? {
        let candidates = allInputDevices()
        if let builtIn = candidates.first(where: { transportTypeGlobal($0) == kAudioDeviceTransportTypeBuiltIn }) {
            return builtIn
        }
        return candidates.first {
            transportTypeGlobal($0) != kAudioDeviceTransportTypeBluetooth
                && transportTypeGlobal($0) != kAudioDeviceTransportTypeBluetoothLE
        }
    }

    static func allOutputDevices() -> [AudioDeviceID] {
        var address = AudioObjectPropertyAddress(
            mSelector: kAudioHardwarePropertyDevices,
            mScope: kAudioObjectPropertyScopeGlobal,
            mElement: kAudioObjectPropertyElementMain
        )
        var size: UInt32 = 0
        guard AudioObjectGetPropertyDataSize(AudioObjectID(kAudioObjectSystemObject), &address, 0, nil, &size) == noErr else {
            return []
        }
        let count = Int(size) / MemoryLayout<AudioDeviceID>.size
        var devices = [AudioDeviceID](repeating: 0, count: count)
        guard AudioObjectGetPropertyData(AudioObjectID(kAudioObjectSystemObject), &address, 0, nil, &size, &devices) == noErr else {
            return []
        }
        // Only devices that actually have output streams.
        return devices.filter { hasOutputStreams($0) }
    }

    private static func hasOutputStreams(_ device: AudioDeviceID) -> Bool {
        var address = AudioObjectPropertyAddress(
            mSelector: kAudioDevicePropertyStreams,
            mScope: kAudioDevicePropertyScopeOutput,
            mElement: kAudioObjectPropertyElementMain
        )
        var size: UInt32 = 0
        guard AudioObjectGetPropertyDataSize(device, &address, 0, nil, &size) == noErr else { return false }
        return size > 0
    }

    static func deviceName(_ device: AudioDeviceID) -> String {
        var name: CFString = "" as CFString
        var size = UInt32(MemoryLayout<CFString>.size)
        var address = AudioObjectPropertyAddress(
            mSelector: kAudioObjectPropertyName,
            mScope: kAudioObjectPropertyScopeGlobal,
            mElement: kAudioObjectPropertyElementMain
        )
        let status = withUnsafeMutablePointer(to: &name) { ptr -> OSStatus in
            AudioObjectGetPropertyData(device, &address, 0, nil, &size, ptr)
        }
        return status == noErr ? (name as String) : "Unknown Device"
    }

    static func transportType(_ device: AudioDeviceID) -> UInt32 {
        var transport: UInt32 = 0
        var size = UInt32(MemoryLayout<UInt32>.size)
        var address = AudioObjectPropertyAddress(
            mSelector: kAudioDevicePropertyTransportType,
            mScope: kAudioDevicePropertyScopeOutput,
            mElement: kAudioObjectPropertyElementMain
        )
        AudioObjectGetPropertyData(device, &address, 0, nil, &size, &transport)
        return transport
    }

    static func dataSourceID(_ device: AudioDeviceID) -> UInt32? {
        var address = AudioObjectPropertyAddress(
            mSelector: kAudioDevicePropertyDataSource,
            mScope: kAudioDevicePropertyScopeOutput,
            mElement: kAudioObjectPropertyElementMain
        )
        guard AudioObjectHasProperty(device, &address) else { return nil }
        var value: UInt32 = 0
        var size = UInt32(MemoryLayout<UInt32>.size)
        let status = AudioObjectGetPropertyData(device, &address, 0, nil, &size, &value)
        return status == noErr ? value : nil
    }

    static func classify(_ device: AudioDeviceID) -> OutputKind {
        let transport = transportType(device)
        if transport == kAudioDeviceTransportTypeBluetooth
            || transport == kAudioDeviceTransportTypeBluetoothLE {
            return .bluetooth
        }
        if transport == kAudioDeviceTransportTypeBuiltIn {
            if let ds = dataSourceID(device), ds == kHeadphoneDataSource {
                return .builtInHeadphones
            }
            return .builtInSpeaker
        }
        return .usbOrOther
    }

    // Uses the "virtual main volume" selector so it works regardless of
    // whether the device exposes a single master channel or per-channel volumes.
    private static var volumeAddress = AudioObjectPropertyAddress(
        mSelector: AudioObjectPropertySelector(kAudioHardwareServiceDeviceProperty_VirtualMainVolume),
        mScope: kAudioDevicePropertyScopeOutput,
        mElement: kAudioObjectPropertyElementMain
    )

    static func volumeIsSettable(_ device: AudioDeviceID) -> Bool {
        var settable: DarwinBoolean = false
        let status = AudioObjectIsPropertySettable(device, &volumeAddress, &settable)
        return status == noErr && settable.boolValue
    }

    static func getVolume(_ device: AudioDeviceID) -> Float32? {
        var volume: Float32 = 0
        var size = UInt32(MemoryLayout<Float32>.size)
        let status = AudioObjectGetPropertyData(device, &volumeAddress, 0, nil, &size, &volume)
        return status == noErr ? volume : nil
    }

    @discardableResult
    static func setVolume(_ device: AudioDeviceID, _ volume: Float32) -> Bool {
        var value = max(0, min(1, volume))
        let size = UInt32(MemoryLayout<Float32>.size)
        let status = AudioObjectSetPropertyData(device, &volumeAddress, 0, nil, size, &value)
        return status == noErr
    }

    // MARK: - Native per-element dB properties

    private static func decibelsAddress(element: AudioObjectPropertyElement) -> AudioObjectPropertyAddress {
        AudioObjectPropertyAddress(
            mSelector: kAudioDevicePropertyVolumeDecibels,
            mScope: kAudioDevicePropertyScopeOutput,
            mElement: element
        )
    }

    private static func decibelsRangeAddress(element: AudioObjectPropertyElement) -> AudioObjectPropertyAddress {
        AudioObjectPropertyAddress(
            mSelector: kAudioDevicePropertyVolumeRangeDecibels,
            mScope: kAudioDevicePropertyScopeOutput,
            mElement: element
        )
    }

    static func decibelsIsSettable(_ device: AudioDeviceID, element: AudioObjectPropertyElement) -> Bool {
        var address = decibelsAddress(element: element)
        guard AudioObjectHasProperty(device, &address) else { return false }
        var settable: DarwinBoolean = false
        let status = AudioObjectIsPropertySettable(device, &address, &settable)
        return status == noErr && settable.boolValue
    }

    static func getVolumeDecibels(_ device: AudioDeviceID, element: AudioObjectPropertyElement) -> Float32? {
        var address = decibelsAddress(element: element)
        var value: Float32 = 0
        var size = UInt32(MemoryLayout<Float32>.size)
        let status = AudioObjectGetPropertyData(device, &address, 0, nil, &size, &value)
        return status == noErr ? value : nil
    }

    @discardableResult
    static func setVolumeDecibels(_ device: AudioDeviceID, element: AudioObjectPropertyElement, _ db: Float32) -> Bool {
        var address = decibelsAddress(element: element)
        var value = db
        let size = UInt32(MemoryLayout<Float32>.size)
        let status = AudioObjectSetPropertyData(device, &address, 0, nil, size, &value)
        return status == noErr
    }

    static func volumeDecibelsRange(_ device: AudioDeviceID, element: AudioObjectPropertyElement) -> ClosedRange<Float32>? {
        var address = decibelsRangeAddress(element: element)
        guard AudioObjectHasProperty(device, &address) else { return nil }
        var range = AudioValueRange(mMinimum: 0, mMaximum: 0)
        var size = UInt32(MemoryLayout<AudioValueRange>.size)
        let status = AudioObjectGetPropertyData(device, &address, 0, nil, &size, &range)
        guard status == noErr, range.mMaximum > range.mMinimum else { return nil }
        return Float32(range.mMinimum)...Float32(range.mMaximum)
    }

    // MARK: - Scalar <-> dB translation (for devices without a native per-element dB property,
    // e.g. many Bluetooth outputs, but whose HAL plugin can still translate a scalar to what dB
    // it corresponds to)

    static func scalarToDecibels(_ device: AudioDeviceID, scalar: Float32) -> Float32? {
        var address = AudioObjectPropertyAddress(
            mSelector: kAudioDevicePropertyVolumeScalarToDecibels,
            mScope: kAudioDevicePropertyScopeOutput,
            mElement: kAudioObjectPropertyElementMain
        )
        guard AudioObjectHasProperty(device, &address) else { return nil }
        var value = scalar
        var size = UInt32(MemoryLayout<Float32>.size)
        let status = AudioObjectGetPropertyData(device, &address, 0, nil, &size, &value)
        return status == noErr ? value : nil
    }

    static func decibelsToScalar(_ device: AudioDeviceID, decibels: Float32) -> Float32? {
        var address = AudioObjectPropertyAddress(
            mSelector: kAudioDevicePropertyVolumeDecibelsToScalar,
            mScope: kAudioDevicePropertyScopeOutput,
            mElement: kAudioObjectPropertyElementMain
        )
        guard AudioObjectHasProperty(device, &address) else { return nil }
        var value = decibels
        var size = UInt32(MemoryLayout<Float32>.size)
        let status = AudioObjectGetPropertyData(device, &address, 0, nil, &size, &value)
        return status == noErr ? value : nil
    }

    // MARK: - Strategy resolution

    enum VolumeControlStrategy {
        // Native per-element dB read/write is supported directly.
        case nativeDecibels(element: AudioObjectPropertyElement, range: ClosedRange<Float32>)
        // No native dB property, but the HAL can translate the virtual scalar to/from dB.
        case scalarTranslation(range: ClosedRange<Float32>)
        // Neither is available; fall back to a plain linear scalar clamp (not true dB).
        case scalarOnly
    }

    static func resolveControlStrategy(for device: AudioDeviceID) -> VolumeControlStrategy {
        for element: AudioObjectPropertyElement in [kAudioObjectPropertyElementMain, 1] {
            if decibelsIsSettable(device, element: element),
               let range = volumeDecibelsRange(device, element: element) {
                return .nativeDecibels(element: element, range: range)
            }
        }

        if let minDb = scalarToDecibels(device, scalar: 0.0),
           let maxDb = scalarToDecibels(device, scalar: 1.0),
           maxDb > minDb,
           decibelsToScalar(device, decibels: maxDb) != nil {
            return .scalarTranslation(range: minDb...maxDb)
        }

        return .scalarOnly
    }
}
