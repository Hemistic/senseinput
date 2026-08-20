#include "audio_io.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <sstream>
#include <thread>
#include <utility>

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4244)
#endif
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"
#ifdef _MSC_VER
#pragma warning(pop)
#endif

#ifdef _WIN32
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <objbase.h>
#include <wrl/client.h>
#endif

namespace {

constexpr std::uint64_t ring_capacity = 16'000 * 30;
constexpr std::size_t worker_chunk_samples = 800;

#ifdef _WIN32
std::string hresult_message(const char* operation, HRESULT result) {
    std::ostringstream output;
    output << operation << " (HRESULT 0x" << std::hex
           << static_cast<unsigned long>(result) << ')';
    return output.str();
}

bool load_audio_with_media_foundation(
    const std::filesystem::path& path,
    std::vector<float>& output,
    std::string& error) {
    using Microsoft::WRL::ComPtr;

    const HRESULT initialize_com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool uninitialize_com = SUCCEEDED(initialize_com);
    if (FAILED(initialize_com) && initialize_com != RPC_E_CHANGED_MODE) {
        error = hresult_message("failed to initialize COM for audio decoding", initialize_com);
        return false;
    }

    const HRESULT startup = MFStartup(MF_VERSION, MFSTARTUP_LITE);
    if (FAILED(startup)) {
        if (uninitialize_com) {
            CoUninitialize();
        }
        error = hresult_message("failed to initialize Media Foundation", startup);
        return false;
    }

    bool succeeded = false;
    do {
        ComPtr<IMFAttributes> attributes;
        HRESULT result = MFCreateAttributes(&attributes, 1);
        if (FAILED(result)) {
            error = hresult_message("failed to create audio decoder attributes", result);
            break;
        }

        ComPtr<IMFSourceReader> reader;
        result = MFCreateSourceReaderFromURL(path.c_str(), attributes.Get(), &reader);
        if (FAILED(result)) {
            error = hresult_message("failed to open audio with Media Foundation", result);
            break;
        }

        ComPtr<IMFMediaType> media_type;
        result = MFCreateMediaType(&media_type);
        if (FAILED(result) ||
            FAILED(result = media_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio)) ||
            FAILED(result = media_type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float)) ||
            FAILED(result = media_type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 1)) ||
            FAILED(result = media_type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 16'000)) ||
            FAILED(result = media_type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 32)) ||
            FAILED(result = media_type->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 4)) ||
            FAILED(result = media_type->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 64'000))) {
            error = hresult_message("failed to configure the audio decoder", result);
            break;
        }
        constexpr DWORD audio_stream =
            static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM);
        constexpr DWORD all_streams =
            static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS);
        result = reader->SetCurrentMediaType(
            audio_stream, nullptr, media_type.Get());
        if (FAILED(result)) {
            error = hresult_message("failed to configure 16 kHz mono decoding", result);
            break;
        }
        reader->SetStreamSelection(all_streams, FALSE);
        reader->SetStreamSelection(audio_stream, TRUE);

        output.clear();
        for (;;) {
            DWORD stream_index = 0;
            DWORD flags = 0;
            LONGLONG timestamp = 0;
            ComPtr<IMFSample> sample;
            result = reader->ReadSample(
                audio_stream,
                0,
                &stream_index,
                &flags,
                &timestamp,
                &sample);
            if (FAILED(result)) {
                error = hresult_message("failed while decoding audio", result);
                break;
            }
            if (sample != nullptr) {
                ComPtr<IMFMediaBuffer> buffer;
                result = sample->ConvertToContiguousBuffer(&buffer);
                if (FAILED(result)) {
                    error = hresult_message("failed to read decoded audio", result);
                    break;
                }
                BYTE* bytes = nullptr;
                DWORD maximum_length = 0;
                DWORD current_length = 0;
                result = buffer->Lock(&bytes, &maximum_length, &current_length);
                if (FAILED(result)) {
                    error = hresult_message("failed to access decoded audio", result);
                    break;
                }
                if (current_length % sizeof(float) != 0) {
                    buffer->Unlock();
                    error = "Media Foundation returned misaligned float audio";
                    break;
                }
                const auto* samples = reinterpret_cast<const float*>(bytes);
                output.insert(
                    output.end(),
                    samples,
                    samples + current_length / sizeof(float));
                buffer->Unlock();
            }
            if ((flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0) {
                succeeded = true;
                break;
            }
        }
    } while (false);

    MFShutdown();
    if (uninitialize_com) {
        CoUninitialize();
    }
    if (succeeded && output.empty()) {
        error = "decoded audio is empty";
        return false;
    }
    return succeeded;
}
#endif

} // namespace

bool load_audio_16khz_mono(
    const std::filesystem::path& path,
    std::vector<float>& output,
    std::string& error) {
    const ma_decoder_config config = ma_decoder_config_init(ma_format_f32, 1, 16'000);
    ma_decoder decoder{};
#ifdef _WIN32
    const ma_result initialize = ma_decoder_init_file_w(path.c_str(), &config, &decoder);
#else
    const ma_result initialize = ma_decoder_init_file(path.string().c_str(), &config, &decoder);
#endif
    if (initialize != MA_SUCCESS) {
#ifdef _WIN32
        return load_audio_with_media_foundation(path, output, error);
#else
        error = "failed to decode audio file";
        return false;
#endif
    }

    output.clear();
    ma_uint64 frame_count = 0;
    if (ma_decoder_get_length_in_pcm_frames(&decoder, &frame_count) == MA_SUCCESS &&
        frame_count > 0) {
        output.resize(static_cast<std::size_t>(frame_count));
        ma_uint64 read = 0;
        const ma_result result = ma_decoder_read_pcm_frames(
            &decoder, output.data(), frame_count, &read);
        if (result != MA_SUCCESS && result != MA_AT_END) {
            ma_decoder_uninit(&decoder);
            error = "failed while decoding audio file";
            return false;
        }
        output.resize(static_cast<std::size_t>(read));
    } else {
        std::vector<float> buffer(16'000);
        for (;;) {
            ma_uint64 read = 0;
            const ma_result result = ma_decoder_read_pcm_frames(
                &decoder, buffer.data(), buffer.size(), &read);
            output.insert(output.end(), buffer.begin(), buffer.begin() + read);
            if (read < buffer.size() || (result != MA_SUCCESS && result != MA_AT_END)) {
                break;
            }
        }
    }
    ma_decoder_uninit(&decoder);
    if (output.empty()) {
#ifdef _WIN32
        return load_audio_with_media_foundation(path, output, error);
#else
        error = "decoded audio is empty";
        return false;
#endif
    }
    return true;
}

struct MicrophoneCapture::Impl {
    ma_device device{};
    bool initialized = false;
    SamplesHandler handler;
    std::vector<float> ring = std::vector<float>(ring_capacity);
    std::atomic<std::uint64_t> write_index{0};
    std::atomic<std::uint64_t> read_index{0};
    std::atomic<std::uint64_t> dropped{0};
    std::atomic<bool> stopping{false};
    std::condition_variable condition;
    std::mutex condition_mutex;
    std::thread worker;
    AudioConditioner conditioner;
    std::atomic<float> input_rms_db{-100.0F};
    std::atomic<float> input_peak_db{-100.0F};
    std::atomic<float> applied_gain_db{0.0F};
    std::atomic<float> clipped_percent{0.0F};

    static void data_callback(
        ma_device* device,
        void*,
        const void* input,
        ma_uint32 frame_count) {
        auto* self = static_cast<Impl*>(device->pUserData);
        if (self == nullptr || input == nullptr || self->stopping.load(std::memory_order_relaxed)) {
            return;
        }
        const auto* samples = static_cast<const float*>(input);
        const std::uint64_t write = self->write_index.load(std::memory_order_relaxed);
        const std::uint64_t read = self->read_index.load(std::memory_order_acquire);
        const std::uint64_t available = ring_capacity - std::min(ring_capacity, write - read);
        const std::uint64_t accepted = std::min<std::uint64_t>(available, frame_count);
        const std::size_t first = static_cast<std::size_t>(write % ring_capacity);
        const std::size_t initial = static_cast<std::size_t>(
            std::min<std::uint64_t>(accepted, ring_capacity - first));
        std::memcpy(&self->ring[first], samples, initial * sizeof(float));
        if (accepted > initial) {
            std::memcpy(
                self->ring.data(),
                samples + initial,
                static_cast<std::size_t>(accepted - initial) * sizeof(float));
        }
        self->write_index.store(write + accepted, std::memory_order_release);
        if (accepted < frame_count) {
            self->dropped.fetch_add(frame_count - accepted, std::memory_order_relaxed);
        }
        self->condition.notify_one();
    }

    void run() {
        std::vector<float> chunk(worker_chunk_samples);
        for (;;) {
            std::uint64_t read = read_index.load(std::memory_order_relaxed);
            const std::uint64_t write = write_index.load(std::memory_order_acquire);
            const std::uint64_t count = std::min<std::uint64_t>(chunk.size(), write - read);
            if (count == 0) {
                if (stopping.load(std::memory_order_acquire)) {
                    return;
                }
                std::unique_lock lock(condition_mutex);
                condition.wait_for(lock, std::chrono::milliseconds(10));
                continue;
            }

            const std::size_t first = static_cast<std::size_t>(read % ring_capacity);
            const std::size_t initial = static_cast<std::size_t>(
                std::min<std::uint64_t>(count, ring_capacity - first));
            std::memcpy(chunk.data(), &ring[first], initial * sizeof(float));
            if (count > initial) {
                std::memcpy(
                    chunk.data() + initial,
                    ring.data(),
                    static_cast<std::size_t>(count - initial) * sizeof(float));
            }
            read_index.store(read + count, std::memory_order_release);
            const AudioLevelMetrics metrics = conditioner.process(
                std::span<float>(chunk.data(), static_cast<std::size_t>(count)));
            input_rms_db.store(metrics.input_rms_db, std::memory_order_relaxed);
            input_peak_db.store(metrics.input_peak_db, std::memory_order_relaxed);
            applied_gain_db.store(metrics.applied_gain_db, std::memory_order_relaxed);
            clipped_percent.store(metrics.clipped_percent, std::memory_order_relaxed);
            handler(std::span<const float>(chunk.data(), static_cast<std::size_t>(count)));
        }
    }
};

MicrophoneCapture::MicrophoneCapture() : impl_(std::make_unique<Impl>()) {}

MicrophoneCapture::~MicrophoneCapture() {
    stop();
}

bool MicrophoneCapture::start(SamplesHandler handler, std::string& error) {
    if (impl_->initialized) {
        error = "microphone capture is already running";
        return false;
    }
    impl_->handler = std::move(handler);
    impl_->stopping.store(false, std::memory_order_release);
    impl_->write_index.store(0, std::memory_order_relaxed);
    impl_->read_index.store(0, std::memory_order_relaxed);
    impl_->dropped.store(0, std::memory_order_relaxed);
    impl_->conditioner.reset();
    impl_->input_rms_db.store(-100.0F, std::memory_order_relaxed);
    impl_->input_peak_db.store(-100.0F, std::memory_order_relaxed);
    impl_->applied_gain_db.store(0.0F, std::memory_order_relaxed);
    impl_->clipped_percent.store(0.0F, std::memory_order_relaxed);

    ma_device_config config = ma_device_config_init(ma_device_type_capture);
    config.capture.format = ma_format_f32;
    config.capture.channels = 1;
    config.sampleRate = 16'000;
    config.dataCallback = Impl::data_callback;
    config.pUserData = impl_.get();
    if (ma_device_init(nullptr, &config, &impl_->device) != MA_SUCCESS) {
        error = "failed to initialize the default microphone";
        return false;
    }
    impl_->initialized = true;
    impl_->worker = std::thread(&Impl::run, impl_.get());
    if (ma_device_start(&impl_->device) != MA_SUCCESS) {
        impl_->stopping.store(true, std::memory_order_release);
        impl_->condition.notify_one();
        impl_->worker.join();
        ma_device_uninit(&impl_->device);
        impl_->initialized = false;
        error = "failed to start the default microphone";
        return false;
    }
    return true;
}

void MicrophoneCapture::stop() {
    if (!impl_->initialized) {
        return;
    }
    ma_device_stop(&impl_->device);
    impl_->stopping.store(true, std::memory_order_release);
    impl_->condition.notify_one();
    if (impl_->worker.joinable()) {
        impl_->worker.join();
    }
    ma_device_uninit(&impl_->device);
    impl_->initialized = false;
}

std::uint64_t MicrophoneCapture::dropped_samples() const {
    return impl_->dropped.load(std::memory_order_relaxed);
}

AudioLevelMetrics MicrophoneCapture::level_metrics() const {
    return {
        .input_rms_db = impl_->input_rms_db.load(std::memory_order_relaxed),
        .input_peak_db = impl_->input_peak_db.load(std::memory_order_relaxed),
        .applied_gain_db = impl_->applied_gain_db.load(std::memory_order_relaxed),
        .clipped_percent = impl_->clipped_percent.load(std::memory_order_relaxed),
    };
}
