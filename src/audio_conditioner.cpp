#include "audio_conditioner.h"

#include <algorithm>
#include <cmath>

namespace {

float amplitude_from_db(float db) {
    return std::pow(10.0F, db / 20.0F);
}

float db_from_amplitude(float amplitude) {
    return amplitude > 1e-5F ? 20.0F * std::log10(amplitude) : -100.0F;
}

} // namespace

AudioConditioner::AudioConditioner(AudioConditionerConfig config) : config_(config) {}

AudioLevelMetrics AudioConditioner::process(std::span<float> samples, int sample_rate) {
    AudioLevelMetrics metrics;
    if (samples.empty()) {
        return metrics;
    }

    double sum_square = 0.0;
    float peak = 0.0F;
    std::size_t clipped = 0;
    for (const float sample : samples) {
        const float magnitude = std::abs(sample);
        peak = std::max(peak, magnitude);
        sum_square += static_cast<double>(sample) * sample;
        if (magnitude >= 0.98F) {
            ++clipped;
        }
    }
    const float rms = static_cast<float>(
        std::sqrt(sum_square / static_cast<double>(samples.size())));
    metrics.input_rms_db = db_from_amplitude(rms);
    metrics.input_peak_db = db_from_amplitude(peak);
    metrics.clipped_percent = static_cast<float>(clipped) * 100.0F / samples.size();

    float desired_gain = 1.0F;
    const float target_rms = amplitude_from_db(config_.target_rms_db);
    const float peak_ceiling = amplitude_from_db(config_.peak_ceiling_db);
    if (rms > target_rms) {
        desired_gain = std::min(desired_gain, target_rms / rms);
    }
    if (peak > peak_ceiling) {
        desired_gain = std::min(desired_gain, peak_ceiling / peak);
    }
    const float minimum_gain = amplitude_from_db(-config_.maximum_attenuation_db);
    desired_gain = std::clamp(desired_gain, minimum_gain, 1.0F);

    if (desired_gain < gain_) {
        gain_ = desired_gain;
    } else {
        const float chunk_ms = static_cast<float>(samples.size()) * 1'000.0F /
            std::max(1, sample_rate);
        const float release = 1.0F - std::exp(-chunk_ms / std::max(1.0F, config_.release_ms));
        gain_ += (desired_gain - gain_) * release;
    }

    for (float& sample : samples) {
        sample = std::clamp(sample, -1.0F, 1.0F) * gain_;
    }
    metrics.applied_gain_db = db_from_amplitude(gain_);
    return metrics;
}

void AudioConditioner::reset() {
    gain_ = 1.0F;
}
