#pragma once

#include <cstddef>
#include <span>
#include <vector>

struct CtcHotword {
    std::vector<int> token_ids;
    float boost = 3.0F;
};

struct CtcHotwordDecodeOptions {
    std::size_t beam_width = 12;
    std::size_t candidates_per_frame = 12;
};

struct CtcHotwordDecodeResult {
    std::vector<int> token_ids;
    bool used_hotword_bias = false;
};

CtcHotwordDecodeResult decode_ctc_with_hotword_bias(
    std::span<const float> logits,
    int frames,
    int vocabulary_size,
    int blank_id,
    std::span<const CtcHotword> hotwords,
    CtcHotwordDecodeOptions options = {});
