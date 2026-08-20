#include "hotword_boost.h"

#include <cassert>
#include <vector>

int main() {
    constexpr int blank = 0;
    constexpr int vocabulary_size = 5;
    const std::vector<float> logits = {
        0.0F, 9.8F, 0.0F, 10.0F, 0.0F,
        0.0F, 0.0F, 9.8F, 0.0F, 10.0F,
        10.0F, 0.0F, 0.0F, 0.0F, 0.0F,
    };

    const CtcHotwordDecodeResult baseline = decode_ctc_with_hotword_bias(
        logits, 3, vocabulary_size, blank, {});
    assert((baseline.token_ids == std::vector<int>{3, 4}));
    assert(!baseline.used_hotword_bias);

    const CtcHotword hotword{{1, 2}, 1.0F};
    const CtcHotwordDecodeResult boosted = decode_ctc_with_hotword_bias(
        logits,
        3,
        vocabulary_size,
        blank,
        std::span<const CtcHotword>(&hotword, 1),
        {.beam_width = 8, .candidates_per_frame = 1});
    assert((boosted.token_ids == std::vector<int>{1, 2}));
    assert(boosted.used_hotword_bias);
    return 0;
}
