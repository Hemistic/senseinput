#pragma once

#include <span>

struct AudioLevelMetrics {
    float input_rms_db = -100.0F;
    float input_peak_db = -100.0F;
    float applied_gain_db = 0.0F;
    float clipped_percent = 0.0F;
};

struct AudioConditionerConfig {
    float target_rms_db = -20.0F;
    float peak_ceiling_db = -4.0F;
    float maximum_attenuation_db = 18.0F;
    float release_ms = 1'500.0F;
};

class AudioConditioner {
public:
    explicit AudioConditioner(AudioConditionerConfig config = {});

    AudioLevelMetrics process(std::span<float> samples, int sample_rate = 16'000);
    void reset();

private:
    AudioConditionerConfig config_;
    float gain_ = 1.0F;
};
