#include "audio_conditioner.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <vector>

int main() {
    AudioConditioner conditioner;

    std::vector<float> quiet(800, 0.01F);
    const AudioLevelMetrics quiet_metrics = conditioner.process(quiet);
    assert(std::abs(quiet_metrics.applied_gain_db) < 0.01F);
    assert(std::abs(quiet.front() - 0.01F) < 0.0001F);

    std::vector<float> loud(800);
    for (std::size_t index = 0; index < loud.size(); ++index) {
        loud[index] = index % 2 == 0 ? 0.8F : -0.8F;
    }
    const AudioLevelMetrics loud_metrics = conditioner.process(loud);
    assert(loud_metrics.applied_gain_db < -10.0F);
    assert(*std::max_element(loud.begin(), loud.end()) <= 0.11F);

    std::vector<float> clipped(800, 1.0F);
    const AudioLevelMetrics clipped_metrics = conditioner.process(clipped);
    assert(clipped_metrics.clipped_percent == 100.0F);
    assert(*std::max_element(clipped.begin(), clipped.end()) < 0.2F);
    return 0;
}
