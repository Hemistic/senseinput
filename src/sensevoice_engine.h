#pragma once

#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

struct HotwordBoostPhrase {
    std::string phrase;
    float boost = 3.0F;
};

struct SenseVoiceResult {
    std::string text;
    int audio_ms = 0;
    int vad_ms = 0;
    int inference_ms = 0;
    bool hotword_bias_applied = false;
};

class SenseVoiceEngine {
public:
    SenseVoiceEngine();
    ~SenseVoiceEngine();

    SenseVoiceEngine(const SenseVoiceEngine&) = delete;
    SenseVoiceEngine& operator=(const SenseVoiceEngine&) = delete;

    bool load(const std::filesystem::path& model_path, int threads, std::string& error);
    [[nodiscard]] bool loaded() const;
    SenseVoiceResult recognize(
        std::span<const float> pcm_16khz_mono,
        std::span<const HotwordBoostPhrase> hotwords = {});

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
