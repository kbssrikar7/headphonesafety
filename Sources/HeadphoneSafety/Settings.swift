import Foundation

final class Settings {
    static let shared = Settings()
    private let defaults = UserDefaults.standard

    private enum Keys {
        static let capEnabled = "capEnabled"
        static let capHeadroomDb = "capHeadroomDb"
        static let limiterEnabled = "limiterEnabled"
    }

    var capEnabled: Bool {
        get { defaults.object(forKey: Keys.capEnabled) as? Bool ?? true }
        set { defaults.set(newValue, forKey: Keys.capEnabled) }
    }

    // How many dB below the active output device's own maximum to cap at.
    // This is a real dB quantity read from the device's HAL, not a linear
    // slider percentage - but it's relative to that device's max output,
    // not a calibrated absolute SPL (macOS doesn't expose per-headphone
    // sensitivity data the way iOS does for Apple/Beats headphones).
    var capHeadroomDb: Int {
        get { defaults.object(forKey: Keys.capHeadroomDb) as? Int ?? 10 }
        set { defaults.set(newValue, forKey: Keys.capHeadroomDb) }
    }

    // Off by default: this reroutes system audio through BlackHole and is a bigger
    // behavior change than the volume cap, so it's an explicit opt-in.
    var limiterEnabled: Bool {
        get { defaults.object(forKey: Keys.limiterEnabled) as? Bool ?? false }
        set { defaults.set(newValue, forKey: Keys.limiterEnabled) }
    }
}
