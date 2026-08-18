#include "Limiter.h"

#include <cmath>
#include <cstring>

namespace hps {
namespace {

constexpr double kAttackMs = 3.0;
constexpr double kReleaseMs = 75.0;

float ComputeCoeff(double timeMs, double sampleRateHz) {
    if (timeMs <= 0.0 || sampleRateHz <= 0.0) return 1.0f;
    return static_cast<float>(1.0 - std::exp(-1.0 / (timeMs * 0.001 * sampleRateHz)));
}

}  // namespace

Limiter::Limiter() : attackCoeff_(1.0f), releaseCoeff_(1.0f) {
    std::memset(envelope_, 0, sizeof(envelope_));
}

void Limiter::Reset(double sampleRateHz, uint32_t channelCount) {
    (void)channelCount;  // envelope_ is always fully cleared, sized for kMaxChannels
    attackCoeff_ = ComputeCoeff(kAttackMs, sampleRateHz);
    releaseCoeff_ = ComputeCoeff(kReleaseMs, sampleRateHz);
    std::memset(envelope_, 0, sizeof(envelope_));
}

void Limiter::Process(float* samples, uint32_t frameCount, uint32_t channelCount,
                       float ceilingLinear) {
    if (!samples || frameCount == 0 || channelCount == 0) return;

    for (uint32_t frame = 0; frame < frameCount; ++frame) {
        for (uint32_t ch = 0; ch < channelCount; ++ch) {
            float* sample = &samples[frame * channelCount + ch];
            if (ch >= kMaxChannels) continue;  // no envelope state - pass through unmodified

            float level = std::fabs(*sample);
            float& env = envelope_[ch];
            if (level > env) {
                env += (level - env) * attackCoeff_;
            } else {
                env += (level - env) * releaseCoeff_;
            }

            float gain = 1.0f;
            if (env > ceilingLinear && env > 0.0f) {
                gain = ceilingLinear / env;
                if (gain > 1.0f) gain = 1.0f;
            }

            *sample *= gain;
        }
    }
}

float HeadroomDbToLinearCeiling(double headroomDb) {
    return static_cast<float>(std::pow(10.0, -headroomDb / 20.0));
}

}  // namespace hps
