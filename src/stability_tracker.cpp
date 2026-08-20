#include "stability_tracker.h"

#include <algorithm>
#include <stdexcept>

namespace {

std::u32string decode_utf8(const std::string& input) {
    std::u32string output;
    output.reserve(input.size());
    for (std::size_t index = 0; index < input.size();) {
        const auto first = static_cast<unsigned char>(input[index]);
        char32_t codepoint = 0;
        std::size_t length = 0;
        if (first < 0x80) {
            codepoint = first;
            length = 1;
        } else if ((first & 0xe0) == 0xc0) {
            codepoint = first & 0x1f;
            length = 2;
        } else if ((first & 0xf0) == 0xe0) {
            codepoint = first & 0x0f;
            length = 3;
        } else if ((first & 0xf8) == 0xf0) {
            codepoint = first & 0x07;
            length = 4;
        } else {
            throw std::runtime_error("invalid UTF-8 leading byte");
        }
        if (index + length > input.size()) {
            throw std::runtime_error("truncated UTF-8 sequence");
        }
        for (std::size_t offset = 1; offset < length; ++offset) {
            const auto next = static_cast<unsigned char>(input[index + offset]);
            if ((next & 0xc0) != 0x80) {
                throw std::runtime_error("invalid UTF-8 continuation byte");
            }
            codepoint = (codepoint << 6) | (next & 0x3f);
        }
        output.push_back(codepoint);
        index += length;
    }
    return output;
}

std::string encode_utf8(const std::u32string& input) {
    std::string output;
    for (const char32_t codepoint : input) {
        if (codepoint <= 0x7f) {
            output.push_back(static_cast<char>(codepoint));
        } else if (codepoint <= 0x7ff) {
            output.push_back(static_cast<char>(0xc0 | (codepoint >> 6)));
            output.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
        } else if (codepoint <= 0xffff) {
            output.push_back(static_cast<char>(0xe0 | (codepoint >> 12)));
            output.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
            output.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
        } else {
            output.push_back(static_cast<char>(0xf0 | (codepoint >> 18)));
            output.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3f)));
            output.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
            output.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
        }
    }
    return output;
}

std::size_t common_prefix_length(const std::deque<std::u32string>& hypotheses) {
    if (hypotheses.empty()) {
        return 0;
    }
    std::size_t length = hypotheses.front().size();
    for (const auto& hypothesis : hypotheses) {
        length = std::min(length, hypothesis.size());
        std::size_t index = 0;
        while (index < length && hypothesis[index] == hypotheses.front()[index]) {
            ++index;
        }
        length = index;
    }
    return length;
}

bool starts_with(const std::u32string& text, const std::u32string& prefix) {
    return text.size() >= prefix.size() && std::equal(prefix.begin(), prefix.end(), text.begin());
}

} // namespace

StabilityTracker::StabilityTracker(std::size_t history_size, std::size_t holdback_chars)
    : history_size_(std::max<std::size_t>(2, history_size)), holdback_chars_(holdback_chars) {}

StableText StabilityTracker::update(const std::string& hypothesis) {
    const auto decoded = decode_utf8(hypothesis);
    history_.push_back(decoded);
    while (history_.size() > history_size_) {
        history_.pop_front();
    }

    bool conflict = !starts_with(decoded, committed_);
    if (!conflict && history_.size() >= 2) {
        const std::size_t common = common_prefix_length(history_);
        const std::size_t safe_length = common > holdback_chars_ ? common - holdback_chars_ : 0;
        if (safe_length > committed_.size()) {
            const std::u32string candidate = decoded.substr(0, safe_length);
            if (starts_with(candidate, committed_)) {
                committed_ = candidate;
            }
        }
    }

    StableText result;
    result.stable = encode_utf8(committed_);
    result.conflict = conflict;
    result.unstable = encode_utf8(starts_with(decoded, committed_)
        ? decoded.substr(committed_.size())
        : decoded);
    return result;
}

StableText StabilityTracker::finalize(const std::string& hypothesis) {
    committed_ = decode_utf8(hypothesis);
    history_.clear();
    return StableText{encode_utf8(committed_), {}, false};
}

void StabilityTracker::reset() {
    history_.clear();
    committed_.clear();
}

