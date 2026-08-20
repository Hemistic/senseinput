#pragma once

#include <cstddef>
#include <deque>
#include <string>

struct StableText {
    std::string stable;
    std::string unstable;
    bool conflict = false;
};

class StabilityTracker {
public:
    explicit StabilityTracker(std::size_t history_size = 3, std::size_t holdback_chars = 3);

    StableText update(const std::string& hypothesis);
    StableText finalize(const std::string& hypothesis);
    void reset();

private:
    std::size_t history_size_;
    std::size_t holdback_chars_;
    std::deque<std::u32string> history_;
    std::u32string committed_;
};

