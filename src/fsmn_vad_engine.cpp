#include "fsmn_vad_engine.h"

#include "ggml-alloc.h"
#include "ggml-backend.h"
#include "ggml-cpu.h"
#include "ggml.h"
#include "gguf.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {

constexpr int sample_rate = 16'000;
constexpr int window_length = 400;
constexpr int frame_shift = 160;
constexpr int fft_size = 512;
constexpr int mel_bins = 80;
constexpr float preemphasis = 0.97F;
constexpr float low_frequency = 20.0F;
constexpr float high_frequency = 8'000.0F;

float mel_frequency(float frequency) {
    return 1127.0F * std::log(1.0F + frequency / 700.0F);
}

void fft(std::vector<float>& real, std::vector<float>& imaginary, int size) {
    for (int index = 1, reverse = 0; index < size; ++index) {
        int bit = size >> 1;
        for (; reverse & bit; bit >>= 1) {
            reverse ^= bit;
        }
        reverse ^= bit;
        if (index < reverse) {
            std::swap(real[index], real[reverse]);
            std::swap(imaginary[index], imaginary[reverse]);
        }
    }
    for (int length = 2; length <= size; length <<= 1) {
        const double angle = -2.0 * 3.14159265358979323846 / length;
        const float rotation_real = std::cos(static_cast<float>(angle));
        const float rotation_imaginary = std::sin(static_cast<float>(angle));
        for (int start = 0; start < size; start += length) {
            float current_real = 1.0F;
            float current_imaginary = 0.0F;
            for (int offset = 0; offset < length / 2; ++offset) {
                const float upper_real = real[start + offset];
                const float upper_imaginary = imaginary[start + offset];
                const float lower_real =
                    real[start + offset + length / 2] * current_real -
                    imaginary[start + offset + length / 2] * current_imaginary;
                const float lower_imaginary =
                    real[start + offset + length / 2] * current_imaginary +
                    imaginary[start + offset + length / 2] * current_real;
                real[start + offset] = upper_real + lower_real;
                imaginary[start + offset] = upper_imaginary + lower_imaginary;
                real[start + offset + length / 2] = upper_real - lower_real;
                imaginary[start + offset + length / 2] = upper_imaginary - lower_imaginary;
                const float next_real =
                    current_real * rotation_real - current_imaginary * rotation_imaginary;
                current_imaginary =
                    current_real * rotation_imaginary + current_imaginary * rotation_real;
                current_real = next_real;
            }
        }
    }
}

std::vector<std::vector<float>> compute_fbank(std::span<const float> input) {
    if (input.size() < window_length) {
        return {};
    }
    std::vector<float> waveform(input.begin(), input.end());
    for (float& sample : waveform) {
        sample *= 32768.0F;
    }
    std::vector<float> window(window_length);
    for (int index = 0; index < window_length; ++index) {
        window[index] = 0.54F - 0.46F *
            std::cos(2.0F * 3.14159265358979323846F * index / (window_length - 1));
    }

    constexpr int spectrum_bins = fft_size / 2 + 1;
    const float bin_width = static_cast<float>(sample_rate) / fft_size;
    const float mel_low = mel_frequency(low_frequency);
    const float mel_high = mel_frequency(high_frequency);
    const float mel_step = (mel_high - mel_low) / (mel_bins + 1);
    std::vector<std::vector<float>> filters(
        mel_bins, std::vector<float>(spectrum_bins, 0.0F));
    for (int mel = 0; mel < mel_bins; ++mel) {
        const float left = mel_low + mel * mel_step;
        const float center = mel_low + (mel + 1) * mel_step;
        const float right = mel_low + (mel + 2) * mel_step;
        for (int bin = 0; bin < spectrum_bins; ++bin) {
            const float frequency = mel_frequency(bin_width * bin);
            if (frequency > left && frequency < right) {
                filters[mel][bin] = frequency <= center
                    ? (frequency - left) / (center - left)
                    : (right - frequency) / (right - center);
            }
        }
    }

    const int frames =
        (static_cast<int>(waveform.size()) - window_length) / frame_shift + 1;
    std::vector<std::vector<float>> features(frames, std::vector<float>(mel_bins));
    std::vector<float> real(fft_size);
    std::vector<float> imaginary(fft_size);
    std::vector<float> frame(window_length);
    constexpr float floor = 1.1920929e-07F;
    for (int time = 0; time < frames; ++time) {
        const float* samples = waveform.data() + time * frame_shift;
        double mean = 0.0;
        for (int index = 0; index < window_length; ++index) {
            mean += samples[index];
        }
        mean /= window_length;
        for (int index = 0; index < window_length; ++index) {
            frame[index] = samples[index] - static_cast<float>(mean);
        }
        for (int index = window_length - 1; index > 0; --index) {
            frame[index] -= preemphasis * frame[index - 1];
        }
        frame[0] -= preemphasis * frame[0];
        for (int index = 0; index < fft_size; ++index) {
            real[index] = index < window_length ? frame[index] * window[index] : 0.0F;
            imaginary[index] = 0.0F;
        }
        fft(real, imaginary, fft_size);
        for (int mel = 0; mel < mel_bins; ++mel) {
            float energy = 0.0F;
            for (int bin = 0; bin < spectrum_bins; ++bin) {
                if (filters[mel][bin] > 0.0F) {
                    energy += filters[mel][bin] *
                        (real[bin] * real[bin] + imaginary[bin] * imaginary[bin]);
                }
            }
            features[time][mel] = std::log(std::max(energy, floor));
        }
    }
    return features;
}

std::vector<float> stack_lfr(
    const std::vector<std::vector<float>>& features,
    int window,
    int stride,
    int& frames) {
    if (features.empty()) {
        frames = 0;
        return {};
    }
    const int source_frames = static_cast<int>(features.size());
    const int padding = (window - 1) / 2;
    frames = (source_frames + stride - 1) / stride;
    std::vector<std::vector<float>> padded;
    padded.reserve(source_frames + padding + window);
    for (int index = 0; index < padding; ++index) {
        padded.push_back(features.front());
    }
    padded.insert(padded.end(), features.begin(), features.end());
    while (static_cast<int>(padded.size()) < (frames - 1) * stride + window) {
        padded.push_back(features.back());
    }
    std::vector<float> output(
        static_cast<std::size_t>(frames) * window * mel_bins);
    for (int time = 0; time < frames; ++time) {
        for (int offset = 0; offset < window; ++offset) {
            std::memcpy(
                &output[(static_cast<std::size_t>(time) * window + offset) * mel_bins],
                padded[time * stride + offset].data(),
                mel_bins * sizeof(float));
        }
    }
    return output;
}

ggml_tensor* linear(
    ggml_context* context,
    ggml_tensor* weights,
    ggml_tensor* bias,
    ggml_tensor* input) {
    ggml_tensor* output = ggml_mul_mat(context, weights, input);
    return bias != nullptr ? ggml_add(context, output, bias) : output;
}

void decode_segments(
    const std::vector<float>& scores,
    std::span<const float> pcm_16khz_mono,
    int frames,
    int output_dimension,
    const VadDetectionOptions& options,
    VadResult& result) {
    constexpr int frame_ms = 10;
    constexpr int frame_samples = 160;
    constexpr int energy_window_samples = 400;
    constexpr int window_frames = 20;
    constexpr int silence_to_speech = 15;
    constexpr int speech_to_silence = 15;
    constexpr int lookahead_end = 10;
    constexpr int start_lookback = window_frames + 20;
    constexpr int chunk_frames = 6'000;
    const int maximum_segment_frames =
        (options.maximum_segment_ms > 0 ? options.maximum_segment_ms : 60'000) / frame_ms;
    const int minimum_speech_frames =
        std::max(1, options.minimum_speech_ms / frame_ms);

    std::vector<float> frame_decibels(static_cast<std::size_t>(frames), -100.0F);
    std::vector<bool> accepted_speech_frames(static_cast<std::size_t>(frames), false);
    for (int time = 0; time < frames; ++time) {
        const std::size_t start = static_cast<std::size_t>(time) * frame_samples;
        const std::size_t end = std::min(
            pcm_16khz_mono.size(), start + energy_window_samples);
        if (end <= start) {
            continue;
        }
        double sum = 0.0;
        for (std::size_t sample = start; sample < end; ++sample) {
            const double value = pcm_16khz_mono[sample];
            sum += value * value;
        }
        const double mean_square = sum / static_cast<double>(end - start);
        frame_decibels[static_cast<std::size_t>(time)] = static_cast<float>(
            10.0 * std::log10(std::max(mean_square, 1e-10)));
    }

    float noise_floor_db = -70.0F;
    bool noise_floor_initialized = false;
    float latest_required_db = options.minimum_db;
    float latest_speech_probability = 0.0F;
    bool latest_model_speech = false;
    bool latest_start_qualified_speech = false;
    result.peak_db = *std::max_element(frame_decibels.begin(), frame_decibels.end());

    int accumulated_ms = 0;
    int in_speech_latched = 0;
    int maximum_end_silence = 0;
    int end_lookback = 0;
    auto recompute_silence = [&] {
        int silence_ms = 0;
        if (accumulated_ms <= 10'000) silence_ms = 2'000;
        else if (accumulated_ms <= 20'000) silence_ms = 1'000;
        else if (accumulated_ms <= 30'000) silence_ms = 800;
        else if (accumulated_ms <= 40'000) silence_ms = 600;
        else if (accumulated_ms <= 50'000) silence_ms = 400;
        else if (accumulated_ms <= 60'000) silence_ms = 200;
        else silence_ms = 100;
        if (options.endpoint_silence_ms > 0) {
            silence_ms = std::min(silence_ms, options.endpoint_silence_ms);
        }
        maximum_end_silence = std::max(0, silence_ms - 150) / frame_ms;
        end_lookback = std::max(0, maximum_end_silence - lookahead_end - 1);
    };
    recompute_silence();

    std::vector<int> window(window_frames, 0);
    int window_position = 0;
    int window_sum = 0;
    int previous_state = 0;
    int state = 0;
    int current_start = -1;
    int current_silence = 0;
    int previous_end = 0;
    struct FrameSegment {
        int start = 0;
        int end = 0;
        bool finalized = false;
    };
    std::vector<FrameSegment> frame_segments;

    auto reset = [&] {
        std::fill(window.begin(), window.end(), 0);
        window_position = 0;
        window_sum = 0;
        previous_state = 0;
        current_silence = 0;
        state = 0;
        current_start = -1;
        accumulated_ms = 0;
        in_speech_latched = 0;
    };
    auto emit = [&](int start, int end, bool finalized) {
        start = std::max(start, previous_end);
        start = std::max(start, 0);
        end = std::min(end, frames);
        if (end > start) {
            frame_segments.push_back({start, end, finalized});
            previous_end = end;
        }
    };

    for (int time = 0; time < frames; ++time) {
        if (time > 0 && time % chunk_frames == 0) {
            if (state == 1 || in_speech_latched != 0) {
                accumulated_ms += 60'000;
                in_speech_latched = 1;
            }
            recompute_silence();
        }
        const float silence = scores[static_cast<std::size_t>(time) * output_dimension];
        const float decibels = frame_decibels[static_cast<std::size_t>(time)];
        const bool model_speech =
            (1.0F - silence) >= silence + options.speech_noise_threshold;
        if (!model_speech) {
            if (!noise_floor_initialized) {
                noise_floor_db = decibels;
                noise_floor_initialized = true;
            } else if (decibels < noise_floor_db) {
                noise_floor_db = decibels;
            } else if (decibels <= noise_floor_db + 6.0F) {
                noise_floor_db = noise_floor_db * 0.98F + decibels * 0.02F;
            }
        }
        const float required_db = std::max(
            options.minimum_db,
            noise_floor_initialized ? noise_floor_db + options.minimum_snr_db : options.minimum_db);
        const bool start_qualified_speech = model_speech && decibels >= required_db;
        result.current_db = decibels;
        latest_required_db = required_db;
        latest_speech_probability = 1.0F - silence;
        latest_model_speech = model_speech;
        latest_start_qualified_speech = start_qualified_speech;
        const int frame_state = state == 0
            ? static_cast<int>(start_qualified_speech)
            : static_cast<int>(model_speech);
        accepted_speech_frames[static_cast<std::size_t>(time)] = start_qualified_speech;
        window_sum -= window[window_position];
        window_sum += frame_state;
        window[window_position] = frame_state;
        window_position = (window_position + 1) % window_frames;

        int change = 0;
        if (previous_state == 0 && window_sum >= silence_to_speech) {
            previous_state = 1;
            change = 3;
        } else if (previous_state == 1 && window_sum <= speech_to_silence) {
            previous_state = 0;
            change = 1;
        } else {
            change = previous_state == 0 ? 0 : 2;
        }

        if (change == 3) {
            current_silence = 0;
            if (state == 0) {
                current_start = std::max(previous_end, time - start_lookback);
                current_start = std::max(current_start, 0);
                state = 1;
            } else if (time - current_start + 1 > maximum_segment_frames) {
                emit(current_start, time, true);
                reset();
            }
        } else if (change == 1 || change == 2) {
            current_silence = 0;
            if (state == 1 && time - current_start + 1 > maximum_segment_frames) {
                emit(current_start, time, true);
                reset();
            }
        } else {
            ++current_silence;
            if (state == 1) {
                if (current_silence >= maximum_end_silence) {
                    emit(current_start, time - end_lookback, true);
                    reset();
                } else if (time - current_start + 1 > maximum_segment_frames) {
                    emit(current_start, time, true);
                    reset();
                }
            }
        }
    }
    if (state == 1) {
        emit(current_start, frames, false);
    }

    result.noise_floor_db = noise_floor_db;
    result.required_db = latest_required_db;
    result.speech_probability = latest_speech_probability;
    if (state == 1 && !latest_model_speech) {
        result.activity = VadActivity::EndpointWait;
    } else if (state == 1) {
        result.activity = VadActivity::Speech;
    } else if (latest_start_qualified_speech || window_sum > 0) {
        result.activity = VadActivity::Candidate;
    } else {
        result.activity = VadActivity::Silence;
    }

    result.segments.reserve(frame_segments.size());
    for (const FrameSegment& segment : frame_segments) {
        int active_frames = 0;
        float peak_db = -100.0F;
        for (int frame = segment.start; frame < segment.end; ++frame) {
            const float decibels = frame_decibels[static_cast<std::size_t>(frame)];
            peak_db = std::max(peak_db, decibels);
            if (accepted_speech_frames[static_cast<std::size_t>(frame)]) {
                ++active_frames;
            }
        }
        if (active_frames < minimum_speech_frames || peak_db < options.minimum_db) {
            continue;
        }
        result.segments.push_back({
            segment.start * frame_ms,
            segment.end * frame_ms,
            segment.finalized,
        });
    }
}

} // namespace

struct FsmnVadEngine::Impl {
    ggml_context* weights_context = nullptr;
    std::map<std::string, ggml_tensor*> tensors;
    ggml_backend_t backend = nullptr;
    ggml_backend_buffer_type_t buffer_type = nullptr;
    int input_dimension = 400;
    int projection_dimension = 128;
    int layers = 4;
    int left_order = 20;
    int output_dimension = 248;
    int lfr_window = 5;
    int lfr_stride = 1;
    int threads = 8;
    bool is_loaded = false;

    ~Impl() {
        if (backend != nullptr) {
            ggml_backend_free(backend);
        }
        if (weights_context != nullptr) {
            ggml_free(weights_context);
        }
    }

    ggml_tensor* get(const std::string& name) const {
        const auto iterator = tensors.find(name);
        if (iterator == tensors.end()) {
            throw std::runtime_error("missing VAD tensor: " + name);
        }
        return iterator->second;
    }
};

FsmnVadEngine::FsmnVadEngine() : impl_(std::make_unique<Impl>()) {}

FsmnVadEngine::~FsmnVadEngine() = default;

bool FsmnVadEngine::load(
    const std::filesystem::path& model_path,
    int threads,
    std::string& error) {
    if (impl_->is_loaded) {
        error = "FSMN-VAD model is already loaded";
        return false;
    }
    impl_->backend = ggml_backend_cpu_init();
    if (impl_->backend == nullptr) {
        error = "failed to initialize FSMN-VAD CPU backend";
        return false;
    }
    impl_->buffer_type = ggml_backend_get_default_buffer_type(impl_->backend);
    gguf_init_params parameters{false, &impl_->weights_context};
    const auto utf8 = model_path.u8string();
    const std::string path(
        reinterpret_cast<const char*>(utf8.data()), utf8.size());
    gguf_context* gguf = gguf_init_from_file(path.c_str(), parameters);
    if (gguf == nullptr) {
        error = "failed to load FSMN-VAD GGUF: " + model_path.string();
        return false;
    }
    auto read_u32 = [&](const char* key, int fallback) {
        const auto index = gguf_find_key(gguf, key);
        return index < 0 ? fallback : static_cast<int>(gguf_get_val_u32(gguf, index));
    };
    impl_->input_dimension = read_u32("vad.input_dim", 400);
    impl_->projection_dimension = read_u32("vad.proj_dim", 128);
    impl_->layers = read_u32("vad.fsmn_layers", 4);
    impl_->left_order = read_u32("vad.lorder", 20);
    impl_->output_dimension = read_u32("vad.output_dim", 248);
    impl_->lfr_window = read_u32("vad.lfr_m", 5);
    impl_->lfr_stride = read_u32("vad.lfr_n", 1);
    const int tensor_count = static_cast<int>(gguf_get_n_tensors(gguf));
    for (int index = 0; index < tensor_count; ++index) {
        const char* name = gguf_get_tensor_name(gguf, index);
        impl_->tensors[name] = ggml_get_tensor(impl_->weights_context, name);
    }
    gguf_free(gguf);
    try {
        impl_->get("cmvn.shift");
        impl_->get("cmvn.scale");
        impl_->get("encoder.in_linear1.linear.weight");
        impl_->get("encoder.in_linear2.linear.weight");
        impl_->get("encoder.out_linear1.linear.weight");
        impl_->get("encoder.out_linear2.linear.weight");
        for (int index = 0; index < impl_->layers; ++index) {
            const std::string prefix = "encoder.fsmn." + std::to_string(index) + ".";
            impl_->get(prefix + "linear.linear.weight");
            impl_->get(prefix + "fsmn_block.conv_left.weight");
            impl_->get(prefix + "affine.linear.weight");
        }
    } catch (const std::exception& exception) {
        error = exception.what();
        return false;
    }
    impl_->threads = std::max(1, threads);
    ggml_backend_cpu_set_n_threads(impl_->backend, impl_->threads);
    impl_->is_loaded = true;
    return true;
}

bool FsmnVadEngine::loaded() const {
    return impl_->is_loaded;
}

VadResult FsmnVadEngine::detect(
    std::span<const float> pcm_16khz_mono,
    const VadDetectionOptions& options) {
    if (!impl_->is_loaded) {
        throw std::runtime_error("FSMN-VAD model is not loaded");
    }
    VadResult result;
    const auto started = std::chrono::steady_clock::now();
    const auto features = compute_fbank(pcm_16khz_mono);
    int frames = 0;
    std::vector<float> stacked = stack_lfr(
        features, impl_->lfr_window, impl_->lfr_stride, frames);
    if (frames == 0) {
        return result;
    }

    const auto* shift = static_cast<const float*>(impl_->get("cmvn.shift")->data);
    const auto* scale = static_cast<const float*>(impl_->get("cmvn.scale")->data);
    for (int time = 0; time < frames; ++time) {
        for (int dimension = 0; dimension < impl_->input_dimension; ++dimension) {
            const std::size_t offset =
                static_cast<std::size_t>(time) * impl_->input_dimension + dimension;
            stacked[offset] = (stacked[offset] + shift[dimension]) * scale[dimension];
        }
    }

    ggml_init_params parameters{
        static_cast<std::size_t>(16) * 1024 * 1024,
        nullptr,
        true,
    };
    ggml_context* context = ggml_init(parameters);
    if (context == nullptr) {
        throw std::runtime_error("failed to allocate FSMN-VAD graph context");
    }
    ggml_gallocr_t allocator = nullptr;
    try {
        ggml_tensor* input = ggml_new_tensor_2d(
            context, GGML_TYPE_F32, impl_->input_dimension, frames);
        ggml_set_input(input);
        ggml_tensor* hidden = linear(
            context,
            impl_->get("encoder.in_linear1.linear.weight"),
            impl_->get("encoder.in_linear1.linear.bias"),
            input);
        hidden = linear(
            context,
            impl_->get("encoder.in_linear2.linear.weight"),
            impl_->get("encoder.in_linear2.linear.bias"),
            hidden);
        hidden = ggml_relu(context, hidden);
        for (int index = 0; index < impl_->layers; ++index) {
            const std::string prefix = "encoder.fsmn." + std::to_string(index) + ".";
            ggml_tensor* projected = ggml_mul_mat(
                context, impl_->get(prefix + "linear.linear.weight"), hidden);
            ggml_tensor* kernel = impl_->get(prefix + "fsmn_block.conv_left.weight");
            ggml_tensor* padded = ggml_pad_ext(
                context, projected, 0, 0, impl_->left_order - 1, 0, 0, 0, 0, 0);
            ggml_tensor* accumulated = projected;
            for (int tap = 0; tap < impl_->left_order; ++tap) {
                ggml_tensor* slice = ggml_view_2d(
                    context,
                    padded,
                    impl_->projection_dimension,
                    frames,
                    padded->nb[1],
                    static_cast<std::size_t>(tap) * padded->nb[1]);
                ggml_tensor* tap_weights = ggml_view_1d(
                    context,
                    kernel,
                    impl_->projection_dimension,
                    static_cast<std::size_t>(tap) * kernel->nb[1]);
                accumulated = ggml_add(
                    context,
                    accumulated,
                    ggml_mul(context, slice, tap_weights));
            }
            hidden = linear(
                context,
                impl_->get(prefix + "affine.linear.weight"),
                impl_->get(prefix + "affine.linear.bias"),
                accumulated);
            hidden = ggml_relu(context, hidden);
        }
        hidden = linear(
            context,
            impl_->get("encoder.out_linear1.linear.weight"),
            impl_->get("encoder.out_linear1.linear.bias"),
            hidden);
        hidden = linear(
            context,
            impl_->get("encoder.out_linear2.linear.weight"),
            impl_->get("encoder.out_linear2.linear.bias"),
            hidden);
        hidden = ggml_soft_max(context, hidden);
        ggml_set_output(hidden);

        ggml_cgraph* graph = ggml_new_graph(context);
        ggml_build_forward_expand(graph, hidden);
        allocator = ggml_gallocr_new(impl_->buffer_type);
        if (!ggml_gallocr_alloc_graph(allocator, graph)) {
            throw std::runtime_error("failed to allocate FSMN-VAD compute graph");
        }
        ggml_backend_tensor_set(input, stacked.data(), 0, ggml_nbytes(input));
        if (ggml_backend_graph_compute(impl_->backend, graph) != GGML_STATUS_SUCCESS) {
            throw std::runtime_error("FSMN-VAD graph computation failed");
        }
        std::vector<float> scores(
            static_cast<std::size_t>(impl_->output_dimension) * frames);
        ggml_backend_tensor_get(hidden, scores.data(), 0, ggml_nbytes(hidden));
        decode_segments(
            scores,
            pcm_16khz_mono,
            frames,
            impl_->output_dimension,
            options,
            result);
    } catch (...) {
        if (allocator != nullptr) {
            ggml_gallocr_free(allocator);
        }
        ggml_free(context);
        throw;
    }
    ggml_gallocr_free(allocator);
    ggml_free(context);
    result.inference_ms = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started)
            .count());
    return result;
}
