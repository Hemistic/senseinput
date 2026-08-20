#include "audio_io.h"
#include "fsmn_vad_engine.h"
#include "sensevoice_engine.h"
#include "stream_recognizer.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

#ifdef _WIN32
using NativeCharacter = wchar_t;
#else
using NativeCharacter = char;
#endif

struct Options {
    std::filesystem::path model;
    std::filesystem::path vad;
    std::optional<std::filesystem::path> audio;
    bool microphone = false;
    int threads = 8;
    int frame_ms = 20;
    int partial_ms = 450;
    int minimum_audio_ms = 600;
    int endpoint_silence_ms = 700;
    int maximum_utterance_ms = 15'000;
    int memory_limit_mb = 300;
    double vad_speech_threshold = 0.55;
    double vad_minimum_db = -60.0;
    double vad_minimum_snr_db = 3.0;
    int vad_minimum_speech_ms = 200;
    bool vad_tuning_explicit = false;
    double hotword_boost = 3.0;
    std::optional<std::filesystem::path> hotwords;
    std::optional<std::filesystem::path> corrections;
    int replay_tail_silence_ms = 0;
    double replay_speed = 1.0;
};

void print_usage() {
    std::cerr
        << "Usage:\n"
        << "  sensevoice-stream --model MODEL.gguf --vad VAD.gguf --audio FILE [options]\n"
        << "  sensevoice-stream --model MODEL.gguf --vad VAD.gguf --mic [options]\n\n"
        << "Options:\n"
        << "  --threads N          ggml CPU threads (default 8)\n"
        << "  --frame-ms N         input frame size in ms (default 20)\n"
        << "  --partial-ms N       target partial interval in ms (default 450)\n"
        << "  --min-audio-ms N     audio required before first partial (default 600)\n"
        << "  --endpoint-silence-ms N  silence required to finalize (default 700)\n"
        << "  --maximum-utterance-ms N  hard sentence budget (default 15000 ms)\n"
        << "  --memory-limit-mb N  working-set protection line (default 300 MB)\n"
        << "  --vad-speech-threshold N  FSMN speech confidence margin\n"
        << "  --vad-min-db N       new-utterance floor in dBFS\n"
        << "  --vad-min-snr-db N   new-utterance margin above noise floor\n"
        << "  --vad-min-speech-ms N  minimum accepted speech duration\n"
        << "                        accuracy defaults: 0.55, -60 dBFS, 3 dB, 200 ms\n"
        << "  --hotwords FILE      tab-separated phrase/aliases dictionary\n"
        << "  --hotword-boost N    CTC hotword bias, 0 disables it (default 3)\n"
        << "  --corrections FILE   tab-separated deterministic correction rules\n"
        << "  --replay-speed N     file replay speed; 1 is real time (default 1)\n"
        << "  --replay-tail-silence-ms N  append silence when testing endpoints\n";
}

bool parse_integer(
    const NativeCharacter* input,
    int minimum,
    int maximum,
    int& output) {
    NativeCharacter* end = nullptr;
#ifdef _WIN32
    const long value = std::wcstol(input, &end, 10);
#else
    const long value = std::strtol(input, &end, 10);
#endif
    if (end == input || *end != '\0' || value < minimum || value > maximum) {
        return false;
    }
    output = static_cast<int>(value);
    return true;
}

bool parse_double(
    const NativeCharacter* input,
    double minimum,
    double maximum,
    double& output) {
    NativeCharacter* end = nullptr;
#ifdef _WIN32
    const double value = std::wcstod(input, &end);
#else
    const double value = std::strtod(input, &end);
#endif
    if (end == input || *end != '\0' || value < minimum || value > maximum) {
        return false;
    }
    output = value;
    return true;
}

std::string option_name(const NativeCharacter* input) {
#ifdef _WIN32
    std::string output;
    for (; *input != L'\0'; ++input) {
        if (*input > 0x7f) {
            return {};
        }
        output.push_back(static_cast<char>(*input));
    }
    return output;
#else
    return input;
#endif
}

std::optional<Options> parse_options(int argc, NativeCharacter** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = option_name(argv[index]);
        auto next = [&]() -> const NativeCharacter* {
            if (index + 1 >= argc) {
                return nullptr;
            }
            return argv[++index];
        };
        if (argument == "--model") {
            const NativeCharacter* value = next();
            if (value == nullptr) {
                return std::nullopt;
            }
            options.model = std::filesystem::path(value);
        } else if (argument == "--vad") {
            const NativeCharacter* value = next();
            if (value == nullptr) {
                return std::nullopt;
            }
            options.vad = std::filesystem::path(value);
        } else if (argument == "--audio") {
            const NativeCharacter* value = next();
            if (value == nullptr) {
                return std::nullopt;
            }
            options.audio = std::filesystem::path(value);
        } else if (argument == "--mic") {
            options.microphone = true;
        } else if (argument == "--threads") {
            const NativeCharacter* value = next();
            if (value == nullptr || !parse_integer(value, 1, 64, options.threads)) {
                return std::nullopt;
            }
        } else if (argument == "--frame-ms") {
            const NativeCharacter* value = next();
            if (value == nullptr || !parse_integer(value, 5, 200, options.frame_ms)) {
                return std::nullopt;
            }
        } else if (argument == "--partial-ms") {
            const NativeCharacter* value = next();
            if (value == nullptr || !parse_integer(value, 50, 5'000, options.partial_ms)) {
                return std::nullopt;
            }
        } else if (argument == "--min-audio-ms") {
            const NativeCharacter* value = next();
            if (value == nullptr || !parse_integer(value, 250, 10'000, options.minimum_audio_ms)) {
                return std::nullopt;
            }
        } else if (argument == "--endpoint-silence-ms") {
            const NativeCharacter* value = next();
            if (value == nullptr ||
                !parse_integer(value, 300, 5'000, options.endpoint_silence_ms)) {
                return std::nullopt;
            }
        } else if (argument == "--maximum-utterance-ms") {
            const NativeCharacter* value = next();
            if (value == nullptr ||
                !parse_integer(value, 1'000, 30'000, options.maximum_utterance_ms)) {
                return std::nullopt;
            }
        } else if (argument == "--memory-limit-mb") {
            const NativeCharacter* value = next();
            if (value == nullptr || !parse_integer(value, 256, 4'096, options.memory_limit_mb)) {
                return std::nullopt;
            }
        } else if (argument == "--vad-speech-threshold") {
            options.vad_tuning_explicit = true;
            const NativeCharacter* value = next();
            if (value == nullptr ||
                !parse_double(value, 0.0, 0.99, options.vad_speech_threshold)) {
                return std::nullopt;
            }
        } else if (argument == "--vad-min-db") {
            options.vad_tuning_explicit = true;
            const NativeCharacter* value = next();
            if (value == nullptr || !parse_double(value, -100.0, -10.0, options.vad_minimum_db)) {
                return std::nullopt;
            }
        } else if (argument == "--vad-min-snr-db") {
            options.vad_tuning_explicit = true;
            const NativeCharacter* value = next();
            if (value == nullptr ||
                !parse_double(value, 0.0, 40.0, options.vad_minimum_snr_db)) {
                return std::nullopt;
            }
        } else if (argument == "--vad-min-speech-ms") {
            options.vad_tuning_explicit = true;
            const NativeCharacter* value = next();
            if (value == nullptr ||
                !parse_integer(value, 100, 2'000, options.vad_minimum_speech_ms)) {
                return std::nullopt;
            }
        } else if (argument == "--hotwords") {
            const NativeCharacter* value = next();
            if (value == nullptr) {
                return std::nullopt;
            }
            options.hotwords = std::filesystem::path(value);
        } else if (argument == "--hotword-boost") {
            const NativeCharacter* value = next();
            if (value == nullptr || !parse_double(value, 0.0, 12.0, options.hotword_boost)) {
                return std::nullopt;
            }
        } else if (argument == "--corrections") {
            const NativeCharacter* value = next();
            if (value == nullptr) {
                return std::nullopt;
            }
            options.corrections = std::filesystem::path(value);
        } else if (argument == "--replay-speed") {
            const NativeCharacter* value = next();
            if (value == nullptr || !parse_double(value, 0.1, 20.0, options.replay_speed)) {
                return std::nullopt;
            }
        } else if (argument == "--replay-tail-silence-ms") {
            const NativeCharacter* value = next();
            if (value == nullptr ||
                !parse_integer(value, 0, 60'000, options.replay_tail_silence_ms)) {
                return std::nullopt;
            }
        } else if (argument == "--help" || argument == "-h") {
            return std::nullopt;
        } else {
            std::cerr << "Unknown option: " << argument << '\n';
            return std::nullopt;
        }
    }
    if (options.model.empty() || options.vad.empty() ||
        options.audio.has_value() == options.microphone) {
        return std::nullopt;
    }
    if (options.microphone && !options.vad_tuning_explicit) {
        options.vad_speech_threshold = 0.55;
        options.vad_minimum_db = -60.0;
        options.vad_minimum_snr_db = 3.0;
        options.vad_minimum_speech_ms = 200;
    }
    return options;
}

std::string json_escape(const std::string& input) {
    std::ostringstream output;
    for (const unsigned char character : input) {
        switch (character) {
        case '"': output << "\\\""; break;
        case '\\': output << "\\\\"; break;
        case '\b': output << "\\b"; break;
        case '\f': output << "\\f"; break;
        case '\n': output << "\\n"; break;
        case '\r': output << "\\r"; break;
        case '\t': output << "\\t"; break;
        default:
            if (character < 0x20) {
                output << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                       << static_cast<int>(character) << std::dec;
            } else {
                output << character;
            }
        }
    }
    return output.str();
}

void print_event(const RecognitionEvent& event, std::mutex& output_mutex) {
    std::scoped_lock lock(output_mutex);
    const char* kind = event.kind == RecognitionEventKind::Partial
        ? "partial"
        : event.kind == RecognitionEventKind::Final ? "final" : "error";
    std::cout << "{\"event\":\"" << kind << "\",\"sequence\":" << event.sequence;
    if (event.kind == RecognitionEventKind::Error) {
        std::cout << ",\"error\":\"" << json_escape(event.error) << "\"";
    } else {
        std::cout << ",\"audio_ms\":" << event.audio_ms
                  << ",\"vad_ms\":" << event.vad_ms
                  << ",\"inference_ms\":" << event.inference_ms
                  << ",\"text\":\"" << json_escape(event.text) << "\""
                  << ",\"stable\":\"" << json_escape(event.stable) << "\""
                  << ",\"unstable\":\"" << json_escape(event.unstable) << "\""
                  << ",\"stability_conflict\":"
                  << (event.stability_conflict ? "true" : "false")
                  << ",\"forced_split\":"
                  << (event.forced_split ? "true" : "false")
                  << ",\"hotword_bias_applied\":"
                  << (event.hotword_bias_applied ? "true" : "false")
                  << ",\"matched_hotwords\":[";
        for (std::size_t index = 0; index < event.matched_hotwords.size(); ++index) {
            if (index != 0) {
                std::cout << ',';
            }
            std::cout << "\"" << json_escape(event.matched_hotwords[index]) << "\"";
        }
        std::cout << "],\"tokens\":[";
        for (std::size_t index = 0; index < event.tokens.size(); ++index) {
            if (index != 0) {
                std::cout << ',';
            }
            std::cout << "\"" << json_escape(event.tokens[index]) << "\"";
        }
        std::cout << "]";
    }
    std::cout << "}" << std::endl;
}

int run_audio_file(const Options& options, StreamRecognizer& recognizer) {
    std::vector<float> audio;
    std::string error;
    if (!load_audio_16khz_mono(*options.audio, audio, error)) {
        std::cerr << error << ": " << options.audio->string() << '\n';
        return 1;
    }
    audio.resize(
        audio.size() + static_cast<std::size_t>(options.replay_tail_silence_ms) * 16,
        0.0F);

    recognizer.start();
    const std::size_t frame_samples = static_cast<std::size_t>(options.frame_ms) * 16;
    const auto replay_started = std::chrono::steady_clock::now();
    std::size_t offset = 0;
    while (offset < audio.size()) {
        const std::size_t count = std::min(frame_samples, audio.size() - offset);
        recognizer.accept_pcm(std::span<const float>(audio.data() + offset, count));
        offset += count;
        const double audio_seconds = static_cast<double>(offset) / 16'000.0;
        const auto target = replay_started + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(audio_seconds / options.replay_speed));
        std::this_thread::sleep_until(target);
    }
    recognizer.finish();
    return 0;
}

int run_microphone(StreamRecognizer& recognizer) {
    MicrophoneCapture microphone;
    std::string error;
    recognizer.start();
    if (!microphone.start(
            [&](std::span<const float> samples) { recognizer.accept_pcm(samples); },
            error)) {
        recognizer.cancel();
        std::cerr << error << '\n';
        return 1;
    }
    std::cerr << "Microphone listening. Press Enter to finalize.\n";
    std::string line;
    std::getline(std::cin, line);
    microphone.stop();
    recognizer.finish();
    if (microphone.dropped_samples() != 0) {
        std::cerr << "warning: microphone ring buffer dropped "
                  << microphone.dropped_samples() << " samples\n";
    }
    return 0;
}

} // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#else
int main(int argc, char** argv) {
#endif
    const auto options = parse_options(argc, argv);
    if (!options.has_value()) {
        print_usage();
        return 2;
    }

    SenseVoiceEngine engine;
    std::string error;
    const auto load_started = std::chrono::steady_clock::now();
    if (!engine.load(options->model, options->threads, error)) {
        std::cerr << error << '\n';
        return 1;
    }
    const auto load_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - load_started).count();
    std::cerr << "SenseVoice Q8 loaded in " << load_ms << " ms\n";

    FsmnVadEngine vad;
    const auto vad_load_started = std::chrono::steady_clock::now();
    if (!vad.load(options->vad, options->threads, error)) {
        std::cerr << error << '\n';
        return 1;
    }
    const auto vad_load_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - vad_load_started).count();
    std::cerr << "FSMN-VAD loaded in " << vad_load_ms << " ms\n";

    std::mutex output_mutex;
    TextProcessor text_processor;
    if (options->hotwords.has_value() &&
        !text_processor.load_hotwords(*options->hotwords, error)) {
        std::cerr << error << '\n';
        return 1;
    }
    if (options->corrections.has_value() &&
        !text_processor.load_correction_rules(*options->corrections, error)) {
        std::cerr << error << '\n';
        return 1;
    }
    const std::filesystem::path executable_directory = [] {
        std::filesystem::path path;
#ifdef _WIN32
        std::wstring buffer(32'768, L'\0');
        const DWORD length = GetModuleFileNameW(
            nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        buffer.resize(length);
        path = std::filesystem::path(buffer);
#else
        path = std::filesystem::current_path() / "sensevoice-stream";
#endif
        return path.parent_path();
    }();
    const std::filesystem::path packaged_dictionary = executable_directory / "dict";
    const std::filesystem::path source_dictionary =
        std::filesystem::current_path() / "third_party" / "cppjieba" / "dict";
    const std::filesystem::path dictionary = std::filesystem::exists(packaged_dictionary)
        ? packaged_dictionary
        : source_dictionary;
    if (std::filesystem::exists(dictionary)) {
        std::string segmenter_error;
        if (!text_processor.initialize_segmenter(dictionary, segmenter_error)) {
            std::cerr << "warning: " << segmenter_error << '\n';
        }
    }
    StreamRecognizerConfig config;
    config.partial_interval_ms = options->partial_ms;
    config.minimum_audio_ms = options->minimum_audio_ms;
    config.endpoint_silence_ms = options->endpoint_silence_ms;
    config.maximum_utterance_ms = options->maximum_utterance_ms;
    config.memory_limit_mb = options->memory_limit_mb;
    config.vad_speech_threshold = static_cast<float>(options->vad_speech_threshold);
    config.vad_minimum_db = static_cast<float>(options->vad_minimum_db);
    config.vad_minimum_snr_db = static_cast<float>(options->vad_minimum_snr_db);
    config.vad_minimum_speech_ms = options->vad_minimum_speech_ms;
    config.hotword_boost = static_cast<float>(options->hotword_boost);
    StreamRecognizer recognizer(
        engine,
        &vad,
        config,
        [&](const RecognitionEvent& event) { print_event(event, output_mutex); },
        &text_processor);

    const int result = options->audio.has_value()
        ? run_audio_file(*options, recognizer)
        : run_microphone(recognizer);
    if (options->hotwords.has_value()) {
        std::string save_error;
        if (!text_processor.save_hotwords(*options->hotwords, save_error)) {
            std::cerr << "warning: " << save_error << '\n';
        }
    }
    return result;
}
