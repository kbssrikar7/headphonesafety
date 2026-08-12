import CoreAudio
import Foundation

// BlackHole (https://github.com/existentialaudio/blackhole) is GPL-3.0 licensed, so it is
// deliberately not bundled or auto-installed by this app - the user installs it themselves
// (e.g. `brew install blackhole-2ch`) and this just detects whether it's present.
enum BlackHoleDetector {
    static func findDevice() -> AudioDeviceID? {
        CoreAudioUtils.allOutputDevices().first { device in
            CoreAudioUtils.deviceName(device).localizedCaseInsensitiveContains("blackhole")
        }
    }

    static var isInstalled: Bool {
        findDevice() != nil
    }
}
