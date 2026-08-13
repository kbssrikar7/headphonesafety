// Startup preferences for Volume Cap and the Real-Time Limiter, persisted to the registry.
//
// Headroom presets mirror macOS's Settings.swift / linux/src/tray.rs::HEADROOM_PRESETS: 0, 5,
// 10, 15, 20 dB below max, default 10.
#pragma once

namespace hps {

constexpr double kHeadroomPresets[] = {0.0, 5.0, 10.0, 15.0, 20.0};
constexpr int kHeadroomPresetCount = 5;

struct Settings {
    bool volumeCapEnabled = true;
    double headroomDb = 10.0;
    bool limiterEnabled = false;

    // Reads from HKCU\Software\HeadphoneSafety, falling back to defaults for any missing value.
    static Settings LoadOrDefault();

    // Writes all three values back to HKCU\Software\HeadphoneSafety.
    void Save() const;
};

} // namespace hps
