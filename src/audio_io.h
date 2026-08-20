#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "audio_conditioner.h"

bool load_audio_16khz_mono(
    const std::filesystem::path& path,
    std::vector<float>& output,
    std::string& error);

class MicrophoneCapture {
public:
    using SamplesHandler = std::function<void(std::span<const float>)>;

    MicrophoneCapture();
    ~MicrophoneCapture();

    MicrophoneCapture(const MicrophoneCapture&) = delete;
    MicrophoneCapture& operator=(const MicrophoneCapture&) = delete;

    bool start(SamplesHandler handler, std::string& error);
    void stop();
    [[nodiscard]] std::uint64_t dropped_samples() const;
    [[nodiscard]] AudioLevelMetrics level_metrics() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
