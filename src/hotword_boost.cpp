#include "hotword_boost.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace {

constexpr float log_zero = -std::numeric_limits<float>::infinity();

struct Candidate {
    int token = 0;
    float score = log_zero;
};

struct Beam {
    std::vector<int> tokens;
    float blank_score = log_zero;
    float non_blank_score = log_zero;
    float bonus = 0.0F;
    bool matched_hotword = false;
};

float log_add(float left, float right) {
    if (left == log_zero) {
        return right;
    }
    if (right == log_zero) {
        return left;
    }
    const float maximum = std::max(left, right);
    return maximum + std::log1p(std::exp(std::min(left, right) - maximum));
}

float beam_score(const Beam& beam) {
    return log_add(beam.blank_score, beam.non_blank_score);
}

std::vector<int> greedy_decode(
    std::span<const float> logits,
    int frames,
    int vocabulary_size,
    int blank_id,
    float& score) {
    std::vector<int> tokens;
    int previous = -1;
    score = 0.0F;
    for (int frame = 0; frame < frames; ++frame) {
        const float* column = logits.data() + static_cast<std::size_t>(frame) * vocabulary_size;
        int maximum = 0;
        float best = column[0];
        for (int token = 1; token < vocabulary_size; ++token) {
            if (column[token] > best) {
                best = column[token];
                maximum = token;
            }
        }
        score += best;
        if (maximum != previous && maximum != blank_id) {
            tokens.push_back(maximum);
        }
        previous = maximum;
    }
    return tokens;
}

std::vector<Candidate> top_candidates(
    const float* column,
    int vocabulary_size,
    std::size_t limit) {
    std::vector<Candidate> candidates;
    candidates.reserve(limit);
    for (int token = 0; token < vocabulary_size; ++token) {
        if (candidates.size() < limit) {
            candidates.push_back({token, column[token]});
            continue;
        }
        const auto minimum = std::min_element(
            candidates.begin(), candidates.end(), [](const Candidate& left, const Candidate& right) {
                return left.score < right.score;
            });
        if (column[token] > minimum->score) {
            *minimum = {token, column[token]};
        }
    }
    std::sort(
        candidates.begin(), candidates.end(), [](const Candidate& left, const Candidate& right) {
            return left.score > right.score;
        });
    return candidates;
}

bool suffix_matches_prefix(
    const std::vector<int>& tokens,
    const std::vector<int>& hotword,
    std::size_t prefix_length) {
    if (prefix_length == 0) {
        return true;
    }
    if (tokens.size() < prefix_length) {
        return false;
    }
    return std::equal(
        hotword.begin(),
        hotword.begin() + static_cast<std::ptrdiff_t>(prefix_length),
        tokens.end() - static_cast<std::ptrdiff_t>(prefix_length));
}

struct HotwordAdvance {
    float bonus = 0.0F;
    bool completed = false;
};

HotwordAdvance advance_hotword(
    const std::vector<int>& tokens,
    int next_token,
    std::span<const CtcHotword> hotwords) {
    HotwordAdvance result;
    for (const CtcHotword& hotword : hotwords) {
        if (hotword.token_ids.empty() || hotword.boost <= 0.0F) {
            continue;
        }
        const std::size_t maximum_prefix = std::min(
            tokens.size(), hotword.token_ids.size() - 1);
        for (std::size_t prefix_length = maximum_prefix + 1; prefix_length-- > 0;) {
            if (hotword.token_ids[prefix_length] != next_token ||
                !suffix_matches_prefix(tokens, hotword.token_ids, prefix_length)) {
                continue;
            }
            const bool completed = prefix_length + 1 == hotword.token_ids.size();
            const float bonus = completed
                ? hotword.boost *
                    (1.0F - 0.25F * static_cast<float>(hotword.token_ids.size() - 1) /
                        static_cast<float>(hotword.token_ids.size()))
                : hotword.boost * 0.25F / static_cast<float>(hotword.token_ids.size());
            if (bonus > result.bonus) {
                result.bonus = bonus;
                result.completed = completed;
            }
            break;
        }
    }
    return result;
}

void append_unique(std::vector<int>& tokens, int token) {
    if (std::find(tokens.begin(), tokens.end(), token) == tokens.end()) {
        tokens.push_back(token);
    }
}

Beam& find_or_create_beam(
    std::vector<Beam>& beams,
    const std::vector<int>& tokens,
    float bonus,
    bool matched_hotword) {
    const auto existing = std::find_if(
        beams.begin(), beams.end(), [&](const Beam& beam) { return beam.tokens == tokens; });
    if (existing != beams.end()) {
        return *existing;
    }
    beams.push_back({tokens, log_zero, log_zero, bonus, matched_hotword});
    return beams.back();
}

} // namespace

CtcHotwordDecodeResult decode_ctc_with_hotword_bias(
    std::span<const float> logits,
    int frames,
    int vocabulary_size,
    int blank_id,
    std::span<const CtcHotword> hotwords,
    CtcHotwordDecodeOptions options) {
    CtcHotwordDecodeResult result;
    if (frames <= 0 || vocabulary_size <= 0 || blank_id < 0 || blank_id >= vocabulary_size ||
        logits.size() < static_cast<std::size_t>(frames) * vocabulary_size) {
        return result;
    }

    float greedy_score = 0.0F;
    result.token_ids = greedy_decode(logits, frames, vocabulary_size, blank_id, greedy_score);
    if (hotwords.empty()) {
        return result;
    }

    options.beam_width = std::max<std::size_t>(options.beam_width, 1);
    options.candidates_per_frame = std::max<std::size_t>(options.candidates_per_frame, 1);
    std::vector<Beam> beams;
    beams.push_back({{}, 0.0F, log_zero, 0.0F, false});

    for (int frame = 0; frame < frames; ++frame) {
        const float* column = logits.data() + static_cast<std::size_t>(frame) * vocabulary_size;
        const std::vector<Candidate> frame_candidates = top_candidates(
            column, vocabulary_size, options.candidates_per_frame);
        std::vector<Beam> next_beams;
        next_beams.reserve(options.beam_width * (frame_candidates.size() + 2));
        for (const Beam& beam : beams) {
            Beam& blank = find_or_create_beam(
                next_beams, beam.tokens, beam.bonus, beam.matched_hotword);
            blank.blank_score = log_add(
                blank.blank_score,
                log_add(beam.blank_score, beam.non_blank_score) + column[blank_id]);

            std::vector<int> candidates;
            candidates.reserve(frame_candidates.size() + hotwords.size());
            for (const Candidate& candidate : frame_candidates) {
                if (candidate.token != blank_id) {
                    append_unique(candidates, candidate.token);
                }
            }
            for (const CtcHotword& hotword : hotwords) {
                if (hotword.token_ids.empty() || hotword.boost <= 0.0F) {
                    continue;
                }
                const std::size_t maximum_prefix = std::min(
                    beam.tokens.size(), hotword.token_ids.size() - 1);
                for (std::size_t prefix_length = maximum_prefix + 1; prefix_length-- > 0;) {
                    if (suffix_matches_prefix(beam.tokens, hotword.token_ids, prefix_length)) {
                        append_unique(candidates, hotword.token_ids[prefix_length]);
                        break;
                    }
                }
            }

            for (const int token : candidates) {
                if (!beam.tokens.empty() && token == beam.tokens.back()) {
                    Beam& unchanged = find_or_create_beam(
                        next_beams, beam.tokens, beam.bonus, beam.matched_hotword);
                    unchanged.non_blank_score = log_add(
                        unchanged.non_blank_score, beam.non_blank_score + column[token]);
                    if (beam.blank_score != log_zero) {
                        std::vector<int> extended = beam.tokens;
                        extended.push_back(token);
                        const HotwordAdvance advance = advance_hotword(
                            beam.tokens, token, hotwords);
                        Beam& appended = find_or_create_beam(
                            next_beams,
                            extended,
                            beam.bonus + advance.bonus,
                            beam.matched_hotword || advance.completed);
                        appended.non_blank_score = log_add(
                            appended.non_blank_score,
                            beam.blank_score + column[token] + advance.bonus);
                    }
                    continue;
                }

                std::vector<int> extended = beam.tokens;
                extended.push_back(token);
                const HotwordAdvance advance = advance_hotword(beam.tokens, token, hotwords);
                Beam& appended = find_or_create_beam(
                    next_beams,
                    extended,
                    beam.bonus + advance.bonus,
                    beam.matched_hotword || advance.completed);
                appended.non_blank_score = log_add(
                    appended.non_blank_score,
                    log_add(beam.blank_score, beam.non_blank_score) + column[token] + advance.bonus);
            }
        }
        std::sort(
            next_beams.begin(), next_beams.end(), [](const Beam& left, const Beam& right) {
                return beam_score(left) > beam_score(right);
            });
        if (next_beams.size() > options.beam_width) {
            next_beams.resize(options.beam_width);
        }
        beams = std::move(next_beams);
    }

    const auto best_hotword = std::max_element(
        beams.begin(), beams.end(), [](const Beam& left, const Beam& right) {
            if (left.matched_hotword != right.matched_hotword) {
                return !left.matched_hotword;
            }
            return beam_score(left) < beam_score(right);
        });
    if (best_hotword != beams.end() && best_hotword->matched_hotword &&
        beam_score(*best_hotword) >= greedy_score) {
        result.token_ids = best_hotword->tokens;
        result.used_hotword_bias = true;
    }
    return result;
}
