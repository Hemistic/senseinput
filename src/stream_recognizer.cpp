#include "stream_recognizer.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <exception>
#include <iterator>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#endif

namespace {

constexpr std::size_t samples_for_ms(int milliseconds) {
    return static_cast<std::size_t>(milliseconds) * 16;
}

std::size_t maximum_audio_samples(const StreamRecognizerConfig& config) {
    return samples_for_ms(std::clamp(config.maximum_utterance_ms, 1'000, 30'000));
}

std::size_t process_working_set_bytes() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX memory{};
    memory.cb = sizeof(memory);
    if (!GetProcessMemoryInfo(
            GetCurrentProcess(),
            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),
            sizeof(memory))) {
        return 0;
    }
    return static_cast<std::size_t>(memory.WorkingSetSize);
#else
    return 0;
#endif
}

bool process_memory_over_limit(int limit_mb, std::size_t baseline_working_set_bytes) {
    if (limit_mb <= 0) {
        return false;
    }
    const std::size_t current_working_set_bytes = process_working_set_bytes();
    if (current_working_set_bytes == 0) {
        return false;
    }

    // The model and dictionaries are resident before the stream starts. Keep the
    // configured line meaningful without allowing that fixed cost to split every
    // utterance immediately on machines whose baseline is already above the line.
    constexpr std::size_t minimum_runtime_headroom_bytes = 32ULL * 1024ULL * 1024ULL;
    const std::size_t configured_limit_bytes =
        static_cast<std::size_t>(limit_mb) * 1024ULL * 1024ULL;
    const std::size_t baseline_aware_limit_bytes =
        baseline_working_set_bytes > 0
        ? baseline_working_set_bytes + minimum_runtime_headroom_bytes
        : configured_limit_bytes;
    const std::size_t effective_limit_bytes =
        std::max(configured_limit_bytes, baseline_aware_limit_bytes);
    return current_working_set_bytes >= effective_limit_bytes;
}

} // namespace

StreamRecognizer::StreamRecognizer(
    SenseVoiceEngine& engine,
    FsmnVadEngine* vad,
    StreamRecognizerConfig config,
    EventHandler handler,
    TextProcessor* text_processor)
    : engine_(engine),
      vad_(vad),
      config_(config),
      handler_(std::move(handler)),
      text_processor_(text_processor),
      stability_(config.stability_history, config.stability_holdback_chars) {}

StreamRecognizer::~StreamRecognizer() {
    cancel();
}

VadResult StreamRecognizer::vad_telemetry() const {
    VadResult result;
    result.current_db = vad_current_db_.load(std::memory_order_relaxed);
    result.peak_db = vad_peak_db_.load(std::memory_order_relaxed);
    result.noise_floor_db = vad_noise_floor_db_.load(std::memory_order_relaxed);
    result.required_db = vad_required_db_.load(std::memory_order_relaxed);
    result.speech_probability = vad_speech_probability_.load(std::memory_order_relaxed);
    result.activity = static_cast<VadActivity>(vad_activity_.load(std::memory_order_relaxed));
    return result;
}

void publish_vad_telemetry(
    const VadResult& result,
    std::atomic<float>& current_db,
    std::atomic<float>& peak_db,
    std::atomic<float>& noise_floor_db,
    std::atomic<float>& required_db,
    std::atomic<float>& speech_probability,
    std::atomic<int>& activity) {
    current_db.store(result.current_db, std::memory_order_relaxed);
    peak_db.store(result.peak_db, std::memory_order_relaxed);
    noise_floor_db.store(result.noise_floor_db, std::memory_order_relaxed);
    required_db.store(result.required_db, std::memory_order_relaxed);
    speech_probability.store(result.speech_probability, std::memory_order_relaxed);
    activity.store(static_cast<int>(result.activity), std::memory_order_relaxed);
}

void StreamRecognizer::start() {
    std::scoped_lock lock(mutex_);
    if (started_) {
        return;
    }
    baseline_working_set_bytes_ = process_working_set_bytes();
    started_ = true;
    worker_ = std::thread(&StreamRecognizer::run, this);
}

void StreamRecognizer::accept_pcm(std::span<const float> samples) {
    if (samples.empty()) {
        return;
    }
    {
        std::scoped_lock lock(mutex_);
        if (!started_ || finish_requested_ || cancel_requested_) {
            return;
        }
        audio_.insert(audio_.end(), samples.begin(), samples.end());
        dirty_ = true;
    }
    condition_.notify_one();
}

void StreamRecognizer::finish() {
    {
        std::scoped_lock lock(mutex_);
        if (!started_) {
            return;
        }
        finish_requested_ = true;
    }
    condition_.notify_one();
    if (worker_.joinable()) {
        worker_.join();
    }
    std::scoped_lock lock(mutex_);
    started_ = false;
}

void StreamRecognizer::cancel() {
    {
        std::scoped_lock lock(mutex_);
        if (!started_) {
            return;
        }
        cancel_requested_ = true;
    }
    condition_.notify_one();
    if (worker_.joinable()) {
        worker_.join();
    }
    std::scoped_lock lock(mutex_);
    started_ = false;
}

void StreamRecognizer::run() {
    using clock = std::chrono::steady_clock;
    auto next_partial = clock::now();

    for (;;) {
        std::vector<float> snapshot;
        std::size_t snapshot_audio_samples = 0;
        bool final_result = false;
        {
            std::unique_lock lock(mutex_);
            condition_.wait_until(lock, next_partial, [&] {
                return cancel_requested_ || finish_requested_ ||
                    (dirty_ && clock::now() >= next_partial);
            });
            if (cancel_requested_) {
                return;
            }
            if (finish_requested_) {
                final_result = true;
                snapshot = audio_;
                snapshot_audio_samples = audio_.size();
            } else {
                const auto minimum_samples = samples_for_ms(config_.minimum_audio_ms);
                const auto new_samples = audio_.size() - last_decoded_samples_;
                if (!dirty_ || audio_.size() < minimum_samples ||
                    new_samples < samples_for_ms(config_.minimum_new_audio_ms)) {
                    next_partial = clock::now() + std::chrono::milliseconds(20);
                    continue;
                }
                snapshot = audio_;
                snapshot_audio_samples = audio_.size();
                dirty_ = false;
                next_partial = clock::now() +
                    std::chrono::milliseconds(std::max(50, config_.partial_interval_ms));
            }
        }

        if (snapshot.empty()) {
            if (final_result) {
                if (!emitted_utterance_) {
                    emit_result(SenseVoiceResult{}, true);
                }
                return;
            }
            continue;
        }

        try {
            VadResult detected_vad;
            const VadResult* vad_result = nullptr;
            if (vad_ != nullptr) {
                detected_vad = vad_->detect(
                    snapshot,
                    VadDetectionOptions{
                        .maximum_segment_ms = std::max(config_.maximum_utterance_ms, 1'000),
                        .endpoint_silence_ms = config_.endpoint_silence_ms,
                        .speech_noise_threshold = config_.vad_speech_threshold,
                        .minimum_db = config_.vad_minimum_db,
                        .minimum_snr_db = config_.vad_minimum_snr_db,
                        .minimum_speech_ms = config_.vad_minimum_speech_ms,
                    });
                vad_result = &detected_vad;
                last_vad_telemetry_ = detected_vad;
                publish_vad_telemetry(
                    detected_vad,
                    vad_current_db_,
                    vad_peak_db_,
                    vad_noise_floor_db_,
                    vad_required_db_,
                    vad_speech_probability_,
                    vad_activity_);
            }

            const std::size_t budget_samples = maximum_audio_samples(config_);
            const bool over_budget = snapshot.size() > budget_samples;
            const bool over_memory = process_memory_over_limit(
                config_.memory_limit_mb,
                baseline_working_set_bytes_);
            if (over_budget || (!final_result && over_memory)) {
                std::size_t split_samples = std::min(budget_samples, snapshot.size());
                if (vad_result != nullptr) {
                    std::size_t nearest_silence = 0;
                    for (const SpeechSegment& segment : vad_result->segments) {
                        const std::size_t end = std::min(
                            snapshot.size(),
                            samples_for_ms(std::max(0, segment.end_ms)));
                        if (end > 0 && end <= budget_samples) {
                            nearest_silence = std::max(nearest_silence, end);
                        }
                    }
                    if (nearest_silence >= samples_for_ms(1'000)) {
                        split_samples = nearest_silence;
                    }
                }
                split_samples = std::min(split_samples, snapshot.size());
                const std::size_t minimum_split_samples = over_memory && !over_budget
                    ? samples_for_ms(3'000)
                    : samples_for_ms(1'000);
                if (split_samples >= minimum_split_samples) {
                    SenseVoiceResult result = recognize_audio(
                        std::span<const float>(snapshot.data(), split_samples));
                    result.audio_ms = static_cast<int>(split_samples / 16);
                    remember_result(result, split_samples);
                    forced_split_ = true;
                    emit_result(result, true);
                    emitted_utterance_ = true;
                    stability_.reset();
                    consume_audio_prefix(split_samples, true);
                    next_partial = clock::now();
                    continue;
                }
            }

            if (!final_result && vad_result != nullptr) {
                const auto endpoint = std::find_if(
                    vad_result->segments.begin(),
                    vad_result->segments.end(),
                    [](const SpeechSegment& segment) { return segment.finalized; });
                if (endpoint != vad_result->segments.end()) {
                    SenseVoiceResult result = recognize_snapshot(snapshot, vad_result, &*endpoint);
                    result.audio_ms = static_cast<int>(snapshot_audio_samples / 16);
                    remember_result(result, snapshot.size());
                    emit_result(result, true);
                    emitted_utterance_ = true;
                    stability_.reset();
                    const auto next_segment = std::next(endpoint);
                    const std::size_t consumed_samples = next_segment == vad_result->segments.end()
                        ? snapshot.size()
                        : samples_for_ms(next_segment->start_ms);
                    consume_audio_prefix(
                        consumed_samples,
                        next_segment != vad_result->segments.end());
                    next_partial = clock::now();
                    continue;
                }
            }

            SenseVoiceResult result;
            if (final_result && snapshot.size() == last_decoded_samples_) {
                result.text = last_text_;
                result.audio_ms = static_cast<int>(snapshot.size() / 16);
                result.vad_ms = last_vad_ms_;
                result.inference_ms = last_inference_ms_;
                result.hotword_bias_applied = last_hotword_bias_applied_;
            } else {
                result = recognize_snapshot(snapshot, vad_result);
            }
            result.audio_ms = static_cast<int>(snapshot_audio_samples / 16);
            remember_result(result, snapshot.size());
            if (!final_result || !result.text.empty() || !emitted_utterance_) {
                emit_result(result, final_result);
            }
            if (!final_result && vad_result != nullptr &&
                vad_result->segments.empty() &&
                snapshot.size() > samples_for_ms(config_.silence_preroll_ms)) {
                consume_audio_prefix(
                    snapshot.size() - samples_for_ms(config_.silence_preroll_ms),
                    false);
            }
        } catch (const std::exception& exception) {
            emit_error(exception.what());
            return;
        }

        if (final_result) {
            return;
        }
    }
}

SenseVoiceResult StreamRecognizer::recognize_snapshot(
    std::span<const float> snapshot,
    const VadResult* detected_vad,
    const SpeechSegment* only_segment) {
    if (vad_ == nullptr) {
        return recognize_audio(snapshot);
    }

    SenseVoiceResult combined;
    combined.audio_ms = static_cast<int>(snapshot.size() / 16);
    VadResult local_vad;
    if (detected_vad == nullptr) {
        local_vad = vad_->detect(
            snapshot,
            VadDetectionOptions{
                .maximum_segment_ms = std::max(config_.maximum_utterance_ms, 1'000),
                .endpoint_silence_ms = config_.endpoint_silence_ms,
                .speech_noise_threshold = config_.vad_speech_threshold,
                .minimum_db = config_.vad_minimum_db,
                .minimum_snr_db = config_.vad_minimum_snr_db,
                .minimum_speech_ms = config_.vad_minimum_speech_ms,
            });
        detected_vad = &local_vad;
    }
    combined.vad_ms = detected_vad->inference_ms;
    const std::span<const SpeechSegment> segments = only_segment != nullptr
        ? std::span<const SpeechSegment>(only_segment, 1)
        : std::span<const SpeechSegment>(detected_vad->segments);
    for (const SpeechSegment& segment : segments) {
        const std::size_t start = std::min(
            snapshot.size(), static_cast<std::size_t>(std::max(0, segment.start_ms)) * 16);
        const std::size_t end = std::min(
            snapshot.size(), static_cast<std::size_t>(std::max(0, segment.end_ms)) * 16);
        if (end <= start || end - start < 400) {
            continue;
        }
        SenseVoiceResult current = recognize_audio(snapshot.subspan(start, end - start));
        if (!combined.text.empty() && !current.text.empty()) {
            const unsigned char left = static_cast<unsigned char>(combined.text.back());
            const unsigned char right = static_cast<unsigned char>(current.text.front());
            if (std::isalnum(left) != 0 && std::isalnum(right) != 0) {
                combined.text.push_back(' ');
            }
        }
        combined.text += current.text;
        combined.inference_ms += current.inference_ms;
        combined.hotword_bias_applied = combined.hotword_bias_applied || current.hotword_bias_applied;
    }
    return combined;
}

SenseVoiceResult StreamRecognizer::recognize_audio(std::span<const float> audio) {
    if (text_processor_ == nullptr || config_.hotword_boost <= 0.0F) {
        return engine_.recognize(audio);
    }
    std::vector<HotwordBoostPhrase> hotwords;
    for (const HotwordEntry& entry : text_processor_->hotwords()) {
        if (entry.enabled && entry.boost > 0.0F) {
            hotwords.push_back({
                entry.phrase,
                std::clamp(entry.boost * config_.hotword_boost / 3.0F, 0.0F, 12.0F),
            });
        }
    }
    return engine_.recognize(audio, hotwords);
}

void StreamRecognizer::consume_audio_prefix(
    std::size_t samples,
    bool reprocess_remaining) {
    std::scoped_lock lock(mutex_);
    const bool has_unprocessed_audio = audio_.size() > last_decoded_samples_;
    samples = std::min(samples, audio_.size());
    audio_.erase(audio_.begin(), audio_.begin() + samples);
    dirty_ = has_unprocessed_audio || reprocess_remaining;
    last_decoded_samples_ = 0;
    last_text_.clear();
    last_vad_ms_ = 0;
    last_inference_ms_ = 0;
    last_hotword_bias_applied_ = false;
}

void StreamRecognizer::remember_result(
    const SenseVoiceResult& result,
    std::size_t decoded_samples) {
    last_decoded_samples_ = decoded_samples;
    last_text_ = result.text;
    last_vad_ms_ = result.vad_ms;
    last_inference_ms_ = result.inference_ms;
    last_hotword_bias_applied_ = result.hotword_bias_applied;
}

void StreamRecognizer::emit_result(const SenseVoiceResult& result, bool final_result) {
    TextProcessResult processed;
    const std::string* display_text = &result.text;
    if (text_processor_ != nullptr) {
        processed = text_processor_->process(result.text, final_result);
        display_text = &processed.text;
    }
    const StableText text = final_result
        ? stability_.finalize(*display_text)
        : stability_.update(*display_text);
    RecognitionEvent event;
    event.kind = final_result ? RecognitionEventKind::Final : RecognitionEventKind::Partial;
    event.sequence = ++sequence_;
    event.audio_ms = result.audio_ms;
    event.vad_ms = result.vad_ms;
    event.inference_ms = result.inference_ms;
    event.vad_current_db = last_vad_telemetry_.current_db;
    event.vad_peak_db = last_vad_telemetry_.peak_db;
    event.vad_noise_floor_db = last_vad_telemetry_.noise_floor_db;
    event.vad_required_db = last_vad_telemetry_.required_db;
    event.vad_speech_probability = last_vad_telemetry_.speech_probability;
    event.vad_activity = last_vad_telemetry_.activity;
    event.text = *display_text;
    event.stable = text.stable;
    event.unstable = text.unstable;
    event.stability_conflict = text.conflict;
    event.forced_split = final_result && forced_split_;
    event.hotword_bias_applied = result.hotword_bias_applied;
    if (final_result) {
        forced_split_ = false;
    }
    if (text_processor_ != nullptr) {
        event.matched_hotwords = std::move(processed.matched_hotwords);
        event.tokens = std::move(processed.tokens);
    }
    handler_(event);
}

void StreamRecognizer::emit_error(const std::string& error) {
    RecognitionEvent event;
    event.kind = RecognitionEventKind::Error;
    event.sequence = ++sequence_;
    event.error = error;
    handler_(event);
}
