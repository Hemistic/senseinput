#pragma once

#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

struct SpeechSegment {
    int start_ms = 0;
    int end_ms = 0;
    bool finalized = false;
};

enum class VadActivity {
    Silence,
    Candidate,
    Speech,
    EndpointWait,
};

struct VadResult {
    std::vector<SpeechSegment> segments;
    int inference_ms = 0;
    float current_db = -100.0F;
    float peak_db = -100.0F;
    float noise_floor_db = -70.0F;
    float required_db = -45.0F;
    float speech_probability = 0.0F;
    VadActivity activity = VadActivity::Silence;
};

struct VadDetectionOptions {
    int maximum_segment_ms = 30'000;
    int endpoint_silence_ms = 2'000;
    float speech_noise_threshold = 0.55F;
    float minimum_db = -60.0F;
    float minimum_snr_db = 3.0F;
    int minimum_speech_ms = 200;
};

class FsmnVadEngine {
public:
    FsmnVadEngine();
    ~FsmnVadEngine();

    FsmnVadEngine(const FsmnVadEngine&) = delete;
    FsmnVadEngine& operator=(const FsmnVadEngine&) = delete;

    bool load(const std::filesystem::path& model_path, int threads, std::string& error);
    [[nodiscard]] bool loaded() const;
    VadResult detect(
        std::span<const float> pcm_16khz_mono,
        const VadDetectionOptions& options = {});

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
