// A simple, non-lookahead envelope-follower peak limiter. Pure DSP math - no Win32/COM calls, no
// allocation inside Process() - safe to call from a real-time audio thread. Promoted here (from
// windows/apo/) as a shared target both windows/apo/ (the parked APO approach, kept as reference)
// and windows/tray/ (LimiterEngine.h/.cpp, the active WASAPI-loopback approach) link against, so
// the DSP algorithm has exactly one copy rather than being duplicated and allowed to drift.
//
// Algorithm matches docs/windows-port.md's "Limiter DSP algorithm" section exactly:
//   1. instantaneous level = |sample|
//   2. envelope follower, fast attack / slower release:
//        if level > envelope: envelope += (level - envelope) * attackCoeff
//        else:                envelope += (level - envelope) * releaseCoeff
//      coeff = 1 - exp(-1 / (timeMs * 0.001 * sampleRate))
//   3. if envelope exceeds ceiling: gain = min(1.0, ceiling / envelope)
//   4. sample *= gain
#pragma once

#include <cstdint>

namespace hps {

class Limiter {
public:
    static constexpr uint32_t kMaxChannels = 8;

    Limiter();

    // Must be called (off the real-time thread) whenever the sample rate or channel count
    // changes, before the first Process() call - resets all envelope state to silence.
    void Reset(double sampleRateHz, uint32_t channelCount);

    // Processes frameCount frames of interleaved float32 samples in place. ceilingLinear is the
    // linear amplitude ceiling (see HeadroomDbToLinearCeiling below). Channels beyond
    // kMaxChannels pass through unmodified (no envelope state for them) - real device streams
    // this project targets are stereo, so this only matters for exotic multichannel setups.
    void Process(float* samples, uint32_t frameCount, uint32_t channelCount, float ceilingLinear);

private:
    float attackCoeff_;
    float releaseCoeff_;
    float envelope_[kMaxChannels];
};

// Converts a headroom-in-dB value (same convention as Volume Cap's dB-below-max presets:
// linux/src/tray.rs::HEADROOM_PRESETS / macOS's Settings.swift) to a linear amplitude ceiling:
// 10^(-headroomDb/20). 0 dB headroom -> ceiling 1.0 (no reduction until 0 dBFS).
float HeadroomDbToLinearCeiling(double headroomDb);

}  // namespace hps
