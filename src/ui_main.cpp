#include "audio_io.h"
#include "fsmn_vad_engine.h"
#include "sensevoice_engine.h"
#include "stream_recognizer.h"

#ifndef _WIN32
#error sensevoice-ui is available only on Windows
#endif

#include <windows.h>
#include <commctrl.h>
#include <psapi.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <chrono>
#include <cstring>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>

namespace {

constexpr UINT message_engine_loaded = WM_APP + 1;
constexpr UINT message_recognition_event = WM_APP + 2;
constexpr UINT message_session_stopped = WM_APP + 3;
constexpr UINT timer_resources = 1;
constexpr UINT timer_meters = 2;

constexpr int id_start_stop = 101;
constexpr int id_copy = 102;
constexpr int id_clear = 103;
constexpr int id_endpoint = 104;
constexpr int id_committed = 105;
constexpr int id_partial = 106;
constexpr int id_status = 107;
constexpr int id_metrics = 108;
constexpr int id_microphone_level = 109;
constexpr int id_vad_level = 110;

constexpr COLORREF color_window = RGB(246, 247, 249);
constexpr COLORREF color_surface = RGB(255, 255, 255);
constexpr COLORREF color_text = RGB(28, 31, 36);
constexpr COLORREF color_muted = RGB(99, 107, 118);
constexpr COLORREF color_accent = RGB(20, 111, 89);
constexpr COLORREF color_danger = RGB(190, 56, 62);
constexpr COLORREF color_border = RGB(218, 222, 228);
constexpr COLORREF color_preview = RGB(72, 80, 91);
constexpr COLORREF color_meter_background = RGB(226, 230, 234);
constexpr COLORREF color_candidate = RGB(201, 139, 31);
constexpr COLORREF color_endpoint = RGB(63, 107, 155);

std::wstring utf8_to_wide(const std::string& input) {
    if (input.empty()) {
        return {};
    }
    const int length = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, input.data(), static_cast<int>(input.size()), nullptr, 0);
    if (length <= 0) {
        return L"[UTF-8 decode error]";
    }
    std::wstring output(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        input.data(),
        static_cast<int>(input.size()),
        output.data(),
        length);
    return output;
}

std::wstring format_duration(int milliseconds) {
    const int seconds = std::max(0, milliseconds / 1'000);
    wchar_t output[32]{};
    swprintf_s(output, L"%02d:%02d", seconds / 60, seconds % 60);
    return output;
}

void set_window_font(HWND window, HFONT font) {
    SendMessageW(window, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
}

class UiApplication {
public:
    explicit UiApplication(HWND window) : window_(window) {}

    ~UiApplication() {
        shutting_down_.store(true, std::memory_order_release);
        stop_session(false);
        if (stopper_.joinable()) {
            stopper_.join();
        }
        if (loader_.joinable()) {
            loader_.join();
        }
        DeleteObject(title_font_);
        DeleteObject(body_font_);
        DeleteObject(small_font_);
        DeleteObject(mono_font_);
        DeleteObject(window_brush_);
        DeleteObject(surface_brush_);
    }

    bool initialize() {
        dpi_ = GetDpiForWindow(window_);
        window_brush_ = CreateSolidBrush(color_window);
        surface_brush_ = CreateSolidBrush(color_surface);
        title_font_ = create_font(25, FW_SEMIBOLD, L"Segoe UI");
        body_font_ = create_font(16, FW_NORMAL, L"Microsoft YaHei UI");
        small_font_ = create_font(13, FW_NORMAL, L"Microsoft YaHei UI");
        mono_font_ = create_font(13, FW_NORMAL, L"Cascadia Mono");
        if (window_brush_ == nullptr || surface_brush_ == nullptr ||
            title_font_ == nullptr || body_font_ == nullptr ||
            small_font_ == nullptr || mono_font_ == nullptr) {
            return false;
        }

        title_ = create_control(L"STATIC", L"SenseVoice 本地听写", SS_LEFT);
        subtitle_ = create_control(
            L"STATIC", L"SenseVoiceSmall Q8 · FSMN-VAD · 全程离线", SS_LEFT);
        status_ = create_control(L"STATIC", L"正在加载模型...", SS_LEFT);
        start_stop_ = create_control(L"BUTTON", L"加载中", BS_OWNERDRAW, id_start_stop);
        copy_ = create_control(L"BUTTON", L"复制", BS_OWNERDRAW, id_copy);
        clear_ = create_control(L"BUTTON", L"清空", BS_OWNERDRAW, id_clear);
        endpoint_label_ = create_control(L"STATIC", L"句尾等待", SS_LEFT);
        endpoint_ = create_control(
            TRACKBAR_CLASSW,
            nullptr,
            TBS_AUTOTICKS | TBS_HORZ,
            id_endpoint);
        endpoint_value_ = create_control(L"STATIC", L"700 ms", SS_RIGHT);
        committed_label_ = create_control(L"STATIC", L"已确认文字", SS_LEFT);
        committed_ = create_control(
            L"EDIT",
            nullptr,
            ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | WS_VSCROLL | WS_TABSTOP,
            id_committed,
            WS_EX_CLIENTEDGE);
        partial_label_ = create_control(L"STATIC", L"正在识别", SS_LEFT);
        partial_ = create_control(
            L"EDIT",
            L"开始听写后，临时结果会显示在这里",
            ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY | WS_VSCROLL,
            id_partial,
            WS_EX_CLIENTEDGE);
        metrics_ = create_control(
            L"STATIC", L"CPU 0%    内存 0 MB    音频 00:00    VAD 0 ms    识别 0 ms", SS_LEFT);
        microphone_label_ = create_control(L"STATIC", L"输入响度", SS_LEFT);
        microphone_meter_ = create_control(
            PROGRESS_CLASSW, nullptr, PBS_SMOOTH, id_microphone_level);
        microphone_value_ = create_control(L"STATIC", L"-∞ dBFS · 峰值 -∞", SS_RIGHT);
        vad_label_ = create_control(L"STATIC", L"VAD 检测", SS_LEFT);
        vad_meter_ = create_control(PROGRESS_CLASSW, nullptr, PBS_SMOOTH, id_vad_level);
        vad_value_ = create_control(
            L"STATIC", L"静音 · 信号 -∞ · 门限 -45 · 噪声 --", SS_RIGHT);

        for (HWND control : {title_, subtitle_, status_, start_stop_, copy_, clear_,
                 endpoint_label_, endpoint_, endpoint_value_, committed_label_, committed_,
                 partial_label_, partial_, metrics_, microphone_label_, microphone_meter_,
                 microphone_value_, vad_label_, vad_meter_, vad_value_}) {
            if (control == nullptr) {
                return false;
            }
        }

        set_window_font(title_, title_font_);
        set_window_font(subtitle_, small_font_);
        set_window_font(status_, small_font_);
        set_window_font(start_stop_, body_font_);
        set_window_font(copy_, body_font_);
        set_window_font(clear_, body_font_);
        set_window_font(endpoint_label_, small_font_);
        set_window_font(endpoint_value_, small_font_);
        set_window_font(committed_label_, small_font_);
        set_window_font(committed_, body_font_);
        set_window_font(partial_label_, small_font_);
        set_window_font(partial_, body_font_);
        set_window_font(metrics_, mono_font_);
        set_window_font(microphone_label_, small_font_);
        set_window_font(microphone_value_, small_font_);
        set_window_font(vad_label_, small_font_);
        set_window_font(vad_value_, small_font_);
        for (HWND meter : {microphone_meter_, vad_meter_}) {
            SendMessageW(meter, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
            SendMessageW(meter, PBM_SETBKCOLOR, 0, color_meter_background);
        }
        SendMessageW(microphone_meter_, PBM_SETBARCOLOR, 0, color_accent);
        SendMessageW(vad_meter_, PBM_SETBARCOLOR, 0, color_muted);

        SendMessageW(endpoint_, TBM_SETRANGE, TRUE, MAKELPARAM(3, 20));
        SendMessageW(endpoint_, TBM_SETTICFREQ, 1, 0);
        SendMessageW(endpoint_, TBM_SETPOS, TRUE, 7);
        EnableWindow(start_stop_, FALSE);
        SetTimer(window_, timer_resources, 1'000, nullptr);
        SetTimer(window_, timer_meters, 50, nullptr);
        last_cpu_sample_ = std::chrono::steady_clock::now();
        GetProcessTimes(
            GetCurrentProcess(),
            &creation_time_,
            &exit_time_,
            &last_kernel_time_,
            &last_user_time_);
        layout();
        begin_load_models();
        return true;
    }

    void layout() const {
        RECT area{};
        GetClientRect(window_, &area);
        const int width = area.right - area.left;
        const int height = area.bottom - area.top;
        const auto scaled = [this](int value) {
            return MulDiv(value, static_cast<int>(dpi_), 96);
        };
        const int margin = scaled(28);
        const int content_width = std::max(200, width - margin * 2);
        const int button_height = scaled(42);
        const int top = scaled(22);

        MoveWindow(title_, margin, top, content_width - scaled(260), scaled(34), TRUE);
        MoveWindow(subtitle_, margin, top + scaled(38), content_width - scaled(260), scaled(24), TRUE);
        MoveWindow(status_, margin, top + scaled(68), content_width - scaled(260), scaled(24), TRUE);

        MoveWindow(start_stop_, width - margin - scaled(176), top, scaled(176), button_height, TRUE);
        MoveWindow(copy_, width - margin - scaled(176), top + scaled(50), scaled(84), scaled(34), TRUE);
        MoveWindow(clear_, width - margin - scaled(84), top + scaled(50), scaled(84), scaled(34), TRUE);

        const int settings_top = top + scaled(105);
        MoveWindow(endpoint_label_, margin, settings_top, scaled(75), scaled(24), TRUE);
        MoveWindow(endpoint_, margin + scaled(82), settings_top - scaled(5), scaled(240), scaled(32), TRUE);
        MoveWindow(endpoint_value_, margin + scaled(328), settings_top, scaled(76), scaled(24), TRUE);

        const int meters_top = settings_top + scaled(38);
        const int meter_label_width = scaled(92);
        const int microphone_value_width = scaled(330);
        const int vad_value_width = scaled(330);
        const int meter_gap = scaled(12);
        const int microphone_meter_width = std::max(
            scaled(100), content_width - meter_label_width - microphone_value_width - meter_gap);
        const int vad_meter_width = std::max(
            scaled(100), content_width - meter_label_width - vad_value_width - meter_gap);
        MoveWindow(
            microphone_label_, margin, meters_top, meter_label_width, scaled(22), TRUE);
        MoveWindow(
            microphone_meter_, margin + meter_label_width, meters_top + scaled(4),
            microphone_meter_width, scaled(14), TRUE);
        MoveWindow(
            microphone_value_, margin + meter_label_width + microphone_meter_width + meter_gap,
            meters_top, microphone_value_width, scaled(22), TRUE);
        MoveWindow(vad_label_, margin, meters_top + scaled(28), meter_label_width, scaled(22), TRUE);
        MoveWindow(
            vad_meter_, margin + meter_label_width, meters_top + scaled(32),
            vad_meter_width, scaled(14), TRUE);
        MoveWindow(
            vad_value_, margin + meter_label_width + vad_meter_width + meter_gap,
            meters_top + scaled(28), vad_value_width, scaled(22), TRUE);

        const int text_top = meters_top + scaled(62);
        const int metrics_height = scaled(30);
        const int metrics_top = height - margin - metrics_height;
        const int labels_and_gaps = scaled(28 + 18 + 28 + 12);
        const int edit_space = std::max(scaled(100), metrics_top - text_top - labels_and_gaps);
        const int committed_height = edit_space * 64 / 100;
        const int partial_height = edit_space - committed_height;
        const int partial_top = text_top + scaled(28) + committed_height + scaled(18);

        MoveWindow(committed_label_, margin, text_top, content_width, scaled(24), TRUE);
        MoveWindow(committed_, margin, text_top + scaled(28), content_width, committed_height, TRUE);
        MoveWindow(partial_label_, margin, partial_top, content_width, scaled(24), TRUE);
        MoveWindow(partial_, margin, partial_top + scaled(28), content_width, partial_height, TRUE);
        MoveWindow(
            metrics_, margin, metrics_top + scaled(6), content_width, metrics_height, TRUE);
    }

    void update_dpi(UINT dpi) {
        if (dpi == 0 || dpi == dpi_) {
            return;
        }
        dpi_ = dpi;
        DeleteObject(title_font_);
        DeleteObject(body_font_);
        DeleteObject(small_font_);
        DeleteObject(mono_font_);
        title_font_ = create_font(25, FW_SEMIBOLD, L"Segoe UI");
        body_font_ = create_font(16, FW_NORMAL, L"Microsoft YaHei UI");
        small_font_ = create_font(13, FW_NORMAL, L"Microsoft YaHei UI");
        mono_font_ = create_font(13, FW_NORMAL, L"Cascadia Mono");
        set_window_font(title_, title_font_);
        set_window_font(subtitle_, small_font_);
        set_window_font(status_, small_font_);
        set_window_font(start_stop_, body_font_);
        set_window_font(copy_, body_font_);
        set_window_font(clear_, body_font_);
        set_window_font(endpoint_label_, small_font_);
        set_window_font(endpoint_value_, small_font_);
        set_window_font(committed_label_, small_font_);
        set_window_font(committed_, body_font_);
        set_window_font(partial_label_, small_font_);
        set_window_font(partial_, body_font_);
        set_window_font(metrics_, mono_font_);
        set_window_font(microphone_label_, small_font_);
        set_window_font(microphone_value_, small_font_);
        set_window_font(vad_label_, small_font_);
        set_window_font(vad_value_, small_font_);
    }

    void on_command(int identifier) {
        if (identifier == id_start_stop) {
            if (state_ == State::Ready) {
                start_session();
            } else if (state_ == State::Listening) {
                stop_session(true);
            }
        } else if (identifier == id_copy) {
            copy_text();
        } else if (identifier == id_clear) {
            SetWindowTextW(committed_, L"");
            SetWindowTextW(partial_, L"");
        }
    }

    void on_horizontal_scroll(HWND source) {
        if (source != endpoint_) {
            return;
        }
        const int milliseconds = static_cast<int>(SendMessageW(endpoint_, TBM_GETPOS, 0, 0)) * 100;
        wchar_t text[32]{};
        swprintf_s(text, L"%d ms", milliseconds);
        SetWindowTextW(endpoint_value_, text);
    }

    void on_engine_loaded(bool success) {
        if (loader_.joinable()) {
            loader_.join();
        }
        if (!success) {
            state_ = State::Error;
            std::wstring error;
            {
                std::scoped_lock lock(model_error_mutex_);
                error = load_error_;
            }
            SetWindowTextW(status_, error.c_str());
            SetWindowTextW(start_stop_, L"不可用");
            InvalidateRect(start_stop_, nullptr, TRUE);
            return;
        }
        state_ = State::Ready;
        SetWindowTextW(status_, L"模型已就绪 · 默认麦克风");
        SetWindowTextW(start_stop_, L"开始听写");
        EnableWindow(start_stop_, TRUE);
        InvalidateRect(start_stop_, nullptr, TRUE);
    }

    void on_recognition_event(RecognitionEvent* owned_event) {
        std::unique_ptr<RecognitionEvent> event(owned_event);
        last_audio_ms_ = event->audio_ms;
        last_vad_ms_ = event->vad_ms;
        last_inference_ms_ = event->inference_ms;
        if (event->kind == RecognitionEventKind::Error) {
            const std::wstring error = utf8_to_wide(event->error);
            SetWindowTextW(status_, error.c_str());
            if (state_ == State::Listening) {
                stop_session(true);
            }
            return;
        }
        const std::wstring text = utf8_to_wide(event->text);
        if (event->kind == RecognitionEventKind::Final) {
            append_committed(text);
            SetWindowTextW(partial_, L"");
        } else {
            SetWindowTextW(partial_, text.c_str());
        }
    }

    void on_session_stopped() {
        if (stopper_.joinable()) {
            stopper_.join();
        }
        microphone_.reset();
        recognizer_.reset();
        if (state_ != State::Error) {
            state_ = State::Ready;
            SetWindowTextW(status_, dropped_samples_ == 0
                    ? L"已停止 · 文字保留在本窗口"
                    : L"已停止 · 检测到音频丢帧");
            SetWindowTextW(start_stop_, L"开始听写");
            EnableWindow(start_stop_, TRUE);
            InvalidateRect(start_stop_, nullptr, TRUE);
        }
    }

    void update_resources() {
        PROCESS_MEMORY_COUNTERS_EX memory{};
        memory.cb = sizeof(memory);
        GetProcessMemoryInfo(
            GetCurrentProcess(),
            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),
            sizeof(memory));

        FILETIME kernel_time{};
        FILETIME user_time{};
        GetProcessTimes(
            GetCurrentProcess(), &creation_time_, &exit_time_, &kernel_time, &user_time);
        const auto now = std::chrono::steady_clock::now();
        const double wall_seconds = std::chrono::duration<double>(now - last_cpu_sample_).count();
        const std::uint64_t cpu_ticks = filetime_value(kernel_time) + filetime_value(user_time) -
            filetime_value(last_kernel_time_) - filetime_value(last_user_time_);
        const unsigned int processors = std::max(1U, std::thread::hardware_concurrency());
        const double cpu_percent = wall_seconds > 0.0
            ? static_cast<double>(cpu_ticks) / 10'000'000.0 / wall_seconds * 100.0 / processors
            : 0.0;
        last_cpu_sample_ = now;
        last_kernel_time_ = kernel_time;
        last_user_time_ = user_time;

        wchar_t text[256]{};
        swprintf_s(
            text,
            L"CPU %.1f%%    内存 %.0f MB    音频 %ls    VAD %d ms    识别 %d ms",
            cpu_percent,
            static_cast<double>(memory.WorkingSetSize) / (1024.0 * 1024.0),
            format_duration(last_audio_ms_).c_str(),
            last_vad_ms_,
            last_inference_ms_);
        SetWindowTextW(metrics_, text);
    }

    static int meter_position(float db) {
        constexpr float minimum_db = -60.0F;
        constexpr float maximum_db = 0.0F;
        return std::clamp(static_cast<int>(
            (db - minimum_db) * 100.0F / (maximum_db - minimum_db)), 0, 100);
    }

    static const wchar_t* vad_activity_text(VadActivity activity) {
        switch (activity) {
        case VadActivity::Candidate: return L"准备";
        case VadActivity::Speech: return L"语音";
        case VadActivity::EndpointWait: return L"等待句尾";
        case VadActivity::Silence:
        default: return L"静音";
        }
    }

    void update_meters() {
        const AudioLevelMetrics microphone_metrics = microphone_ != nullptr
            ? microphone_->level_metrics()
            : AudioLevelMetrics{};
        const float db = microphone_metrics.input_rms_db;
        SendMessageW(microphone_meter_, PBM_SETPOS, meter_position(db), 0);
        wchar_t microphone_text[160]{};
        if (microphone_metrics.clipped_percent >= 0.1F) {
            swprintf_s(
                microphone_text,
                L"输入削波 %.1f%% · 请降低系统麦克风音量",
                microphone_metrics.clipped_percent);
            SendMessageW(microphone_meter_, PBM_SETBARCOLOR, 0, color_danger);
        } else if (microphone_metrics.applied_gain_db <= -0.5F) {
            swprintf_s(
                microphone_text,
                L"%+.1f dBFS · 峰值 %+.1f · 自动衰减 %.1f dB",
                db,
                microphone_metrics.input_peak_db,
                -microphone_metrics.applied_gain_db);
            SendMessageW(microphone_meter_, PBM_SETBARCOLOR, 0, color_candidate);
        } else if (db <= -99.5F) {
            swprintf_s(microphone_text, L"-∞ dBFS · 峰值 -∞");
            SendMessageW(microphone_meter_, PBM_SETBARCOLOR, 0, color_accent);
        } else {
            swprintf_s(
                microphone_text,
                L"%+.1f dBFS · 峰值 %+.1f",
                db,
                microphone_metrics.input_peak_db);
            SendMessageW(microphone_meter_, PBM_SETBARCOLOR, 0, color_accent);
        }
        SetWindowTextW(microphone_value_, microphone_text);

        VadResult telemetry;
        if (recognizer_ != nullptr) {
            telemetry = recognizer_->vad_telemetry();
        }
        const int vad_position = meter_position(telemetry.current_db);
        SendMessageW(vad_meter_, PBM_SETPOS, vad_position, 0);
        const COLORREF vad_color = telemetry.activity == VadActivity::Speech
            ? color_accent
            : telemetry.activity == VadActivity::Candidate
                ? color_candidate
                : telemetry.activity == VadActivity::EndpointWait
                    ? color_endpoint
                    : color_muted;
        SendMessageW(vad_meter_, PBM_SETBARCOLOR, 0, vad_color);
        wchar_t vad_text[160]{};
        swprintf_s(
            vad_text,
            L"%ls · 信号 %+.1f · 阈 %+.1f · 底噪 %+.1f dB",
            vad_activity_text(telemetry.activity),
            telemetry.current_db,
            telemetry.required_db,
            telemetry.noise_floor_db);
        SetWindowTextW(vad_value_, vad_text);
    }

    HBRUSH brush_for_control(HWND control, HDC device_context) const {
        if (control == committed_ || control == partial_) {
            SetBkColor(device_context, color_surface);
            SetTextColor(device_context, control == partial_ ? color_preview : color_text);
            return surface_brush_;
        }
        SetBkColor(device_context, color_window);
        SetTextColor(device_context, control == status_ || control == subtitle_ || control == metrics_
                ? color_muted
                : color_text);
        return window_brush_;
    }

    void draw_button(const DRAWITEMSTRUCT& item) const {
        const bool primary = item.CtlID == id_start_stop;
        const bool disabled = (item.itemState & ODS_DISABLED) != 0;
        const bool pressed = (item.itemState & ODS_SELECTED) != 0;
        COLORREF fill = color_surface;
        COLORREF border = color_border;
        COLORREF text_color = color_text;
        if (primary) {
            fill = state_ == State::Listening ? color_danger : color_accent;
            border = fill;
            text_color = RGB(255, 255, 255);
        }
        if (disabled) {
            fill = RGB(224, 227, 231);
            border = fill;
            text_color = color_muted;
        } else if (pressed) {
            fill = primary ? RGB(15, 87, 70) : RGB(232, 235, 239);
        }

        HBRUSH brush = CreateSolidBrush(fill);
        HPEN pen = CreatePen(PS_SOLID, 1, border);
        const auto old_brush = SelectObject(item.hDC, brush);
        const auto old_pen = SelectObject(item.hDC, pen);
        RoundRect(
            item.hDC,
            item.rcItem.left,
            item.rcItem.top,
            item.rcItem.right,
            item.rcItem.bottom,
            8,
            8);
        SelectObject(item.hDC, old_brush);
        SelectObject(item.hDC, old_pen);
        DeleteObject(brush);
        DeleteObject(pen);

        wchar_t label[64]{};
        GetWindowTextW(item.hwndItem, label, static_cast<int>(std::size(label)));
        SetBkMode(item.hDC, TRANSPARENT);
        SetTextColor(item.hDC, text_color);
        SelectObject(item.hDC, body_font_);
        RECT text_area = item.rcItem;
        DrawTextW(item.hDC, label, -1, &text_area, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

private:
    enum class State {
        Loading,
        Ready,
        Listening,
        Stopping,
        Error,
    };

    HFONT create_font(int points, int weight, const wchar_t* family) const {
        const int height = -MulDiv(points, static_cast<int>(dpi_), 72);
        return CreateFontW(
            height,
            0,
            0,
            0,
            weight,
            FALSE,
            FALSE,
            FALSE,
            DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE,
            family);
    }

    HWND create_control(
        const wchar_t* class_name,
        const wchar_t* text,
        DWORD style,
        int identifier = 0,
        DWORD extended_style = 0) const {
        return CreateWindowExW(
            extended_style,
            class_name,
            text,
            WS_CHILD | WS_VISIBLE | style,
            0,
            0,
            0,
            0,
            window_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(identifier)),
            GetModuleHandleW(nullptr),
            nullptr);
    }

    void begin_load_models() {
        loader_ = std::thread([this] {
            const std::filesystem::path executable = [] {
                std::wstring path(32'768, L'\0');
                const DWORD length = GetModuleFileNameW(
                    nullptr, path.data(), static_cast<DWORD>(path.size()));
                path.resize(length);
                return std::filesystem::path(path);
            }();
            const std::filesystem::path models = executable.parent_path() / L"models";
            const std::filesystem::path dictionary = executable.parent_path() / L"dict";
            const std::filesystem::path hotwords = executable.parent_path() / L"hotwords.tsv";
            const std::filesystem::path corrections = executable.parent_path() / L"corrections.tsv";
            std::string error;
            const bool success = engine_.load(
                    models / L"sensevoice-small-q8.gguf", 8, error) &&
                vad_.load(models / L"fsmn-vad.gguf", 8, error);
            if (success && std::filesystem::exists(hotwords) &&
                !text_processor_.load_hotwords(hotwords, error)) {
                std::scoped_lock lock(model_error_mutex_);
                load_error_ = utf8_to_wide(error);
            }
            if (success && std::filesystem::exists(corrections) &&
                !text_processor_.load_correction_rules(corrections, error)) {
                std::scoped_lock lock(model_error_mutex_);
                load_error_ = utf8_to_wide(error);
            }
            if (success && std::filesystem::exists(dictionary)) {
                std::string segmenter_error;
                if (!text_processor_.initialize_segmenter(dictionary, segmenter_error)) {
                    std::scoped_lock lock(model_error_mutex_);
                    load_error_ = utf8_to_wide(segmenter_error);
                }
            }
            if (!success) {
                std::scoped_lock lock(model_error_mutex_);
                load_error_ = utf8_to_wide(error);
            }
            if (!shutting_down_.load(std::memory_order_acquire)) {
                PostMessageW(window_, message_engine_loaded, success ? TRUE : FALSE, 0);
            }
        });
    }

    void start_session() {
        if (state_ != State::Ready) {
            return;
        }
        recognizer_ = std::make_unique<StreamRecognizer>(
            engine_,
            &vad_,
            StreamRecognizerConfig{
                .partial_interval_ms = 450,
                .minimum_audio_ms = 600,
                .minimum_new_audio_ms = 240,
                .endpoint_silence_ms = static_cast<int>(
                    SendMessageW(endpoint_, TBM_GETPOS, 0, 0)) * 100,
                .maximum_utterance_ms = 15'000,
                .memory_limit_mb = 300,
                .vad_speech_threshold = 0.55F,
                .vad_minimum_db = -60.0F,
                .vad_minimum_snr_db = 3.0F,
                .vad_minimum_speech_ms = 200,
            },
            [this](const RecognitionEvent& event) {
                if (shutting_down_.load(std::memory_order_acquire)) {
                    return;
                }
                auto* owned = new RecognitionEvent(event);
                if (!PostMessageW(
                        window_, message_recognition_event, 0, reinterpret_cast<LPARAM>(owned))) {
                    delete owned;
                }
            },
            &text_processor_);
        microphone_ = std::make_unique<MicrophoneCapture>();
        recognizer_->start();
        std::string error;
        if (!microphone_->start(
                [this](std::span<const float> samples) {
                    recognizer_->accept_pcm(samples);
                },
                error)) {
            recognizer_->cancel();
            recognizer_.reset();
            microphone_.reset();
            state_ = State::Ready;
            const std::wstring message = utf8_to_wide(error);
            SetWindowTextW(status_, message.c_str());
            SetWindowTextW(start_stop_, L"重试");
            EnableWindow(start_stop_, TRUE);
            InvalidateRect(start_stop_, nullptr, TRUE);
            return;
        }
        state_ = State::Listening;
        last_audio_ms_ = 0;
        dropped_samples_ = 0;
        SetWindowTextW(status_, L"正在听写 · 停顿后自动确认");
        SetWindowTextW(start_stop_, L"停止听写");
        SetWindowTextW(partial_, L"正在聆听...");
        EnableWindow(endpoint_, FALSE);
        InvalidateRect(start_stop_, nullptr, TRUE);
    }

    void stop_session(bool notify) {
        if (state_ != State::Listening || microphone_ == nullptr || recognizer_ == nullptr) {
            return;
        }
        state_ = State::Stopping;
        SetWindowTextW(status_, L"正在完成最后一句...");
        SetWindowTextW(start_stop_, L"正在停止");
        EnableWindow(start_stop_, FALSE);
        EnableWindow(endpoint_, TRUE);
        InvalidateRect(start_stop_, nullptr, TRUE);
        stopper_ = std::thread([this, notify] {
            microphone_->stop();
            dropped_samples_ = microphone_->dropped_samples();
            recognizer_->finish();
            if (notify && !shutting_down_.load(std::memory_order_acquire)) {
                PostMessageW(window_, message_session_stopped, 0, 0);
            }
        });
        if (!notify && stopper_.joinable()) {
            stopper_.join();
        }
    }

    void append_committed(const std::wstring& text) const {
        if (text.empty()) {
            return;
        }
        const int existing_length = GetWindowTextLengthW(committed_);
        SendMessageW(committed_, EM_SETSEL, existing_length, existing_length);
        if (existing_length > 0) {
            SendMessageW(committed_, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(L"\r\n"));
        }
        SendMessageW(
            committed_, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(text.c_str()));
        SendMessageW(committed_, EM_SCROLLCARET, 0, 0);
    }

    void copy_text() const {
        const int length = GetWindowTextLengthW(committed_);
        if (length <= 0) {
            return;
        }
        std::wstring text(static_cast<std::size_t>(length + 1), L'\0');
        GetWindowTextW(committed_, text.data(), length + 1);
        const std::size_t bytes = static_cast<std::size_t>(length + 1) * sizeof(wchar_t);
        HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (memory == nullptr || !OpenClipboard(window_)) {
            if (memory != nullptr) {
                GlobalFree(memory);
            }
            return;
        }
        void* destination = GlobalLock(memory);
        if (destination == nullptr) {
            CloseClipboard();
            GlobalFree(memory);
            return;
        }
        memcpy(destination, text.c_str(), bytes);
        GlobalUnlock(memory);
        EmptyClipboard();
        if (SetClipboardData(CF_UNICODETEXT, memory) == nullptr) {
            GlobalFree(memory);
        }
        CloseClipboard();
    }

    static std::uint64_t filetime_value(const FILETIME& time) {
        ULARGE_INTEGER value{};
        value.LowPart = time.dwLowDateTime;
        value.HighPart = time.dwHighDateTime;
        return value.QuadPart;
    }

    HWND window_ = nullptr;
    HWND title_ = nullptr;
    HWND subtitle_ = nullptr;
    HWND status_ = nullptr;
    HWND start_stop_ = nullptr;
    HWND copy_ = nullptr;
    HWND clear_ = nullptr;
    HWND endpoint_label_ = nullptr;
    HWND endpoint_ = nullptr;
    HWND endpoint_value_ = nullptr;
    HWND committed_label_ = nullptr;
    HWND committed_ = nullptr;
    HWND partial_label_ = nullptr;
    HWND partial_ = nullptr;
    HWND metrics_ = nullptr;
    HWND microphone_label_ = nullptr;
    HWND microphone_meter_ = nullptr;
    HWND microphone_value_ = nullptr;
    HWND vad_label_ = nullptr;
    HWND vad_meter_ = nullptr;
    HWND vad_value_ = nullptr;

    HFONT title_font_ = nullptr;
    HFONT body_font_ = nullptr;
    HFONT small_font_ = nullptr;
    HFONT mono_font_ = nullptr;
    HBRUSH window_brush_ = nullptr;
    HBRUSH surface_brush_ = nullptr;

    SenseVoiceEngine engine_;
    FsmnVadEngine vad_;
    TextProcessor text_processor_;
    std::unique_ptr<StreamRecognizer> recognizer_;
    std::unique_ptr<MicrophoneCapture> microphone_;
    std::thread loader_;
    std::thread stopper_;
    std::atomic<bool> shutting_down_{false};
    std::mutex model_error_mutex_;
    std::wstring load_error_;
    State state_ = State::Loading;
    std::uint64_t dropped_samples_ = 0;
    int last_audio_ms_ = 0;
    int last_vad_ms_ = 0;
    int last_inference_ms_ = 0;
    std::chrono::steady_clock::time_point last_cpu_sample_;
    FILETIME creation_time_{};
    FILETIME exit_time_{};
    FILETIME last_kernel_time_{};
    FILETIME last_user_time_{};
    UINT dpi_ = 96;
};

LRESULT CALLBACK window_procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* application = reinterpret_cast<UiApplication*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    switch (message) {
    case WM_CREATE: {
        auto created = std::make_unique<UiApplication>(window);
        if (!created->initialize()) {
            return -1;
        }
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(created.release()));
        return 0;
    }
    case WM_SIZE:
        if (application != nullptr) {
            application->layout();
        }
        return 0;
    case WM_GETMINMAXINFO: {
        const UINT dpi = GetDpiForWindow(window);
        auto* limits = reinterpret_cast<MINMAXINFO*>(lparam);
        limits->ptMinTrackSize.x = MulDiv(760, static_cast<int>(dpi), 96);
        limits->ptMinTrackSize.y = MulDiv(480, static_cast<int>(dpi), 96);
        return 0;
    }
    case WM_DPICHANGED:
        if (application != nullptr) {
            application->update_dpi(HIWORD(wparam));
            const auto* suggested = reinterpret_cast<const RECT*>(lparam);
            SetWindowPos(
                window,
                nullptr,
                suggested->left,
                suggested->top,
                suggested->right - suggested->left,
                suggested->bottom - suggested->top,
                SWP_NOACTIVATE | SWP_NOZORDER);
            application->layout();
        }
        return 0;
    case WM_COMMAND:
        if (application != nullptr && HIWORD(wparam) == BN_CLICKED) {
            application->on_command(LOWORD(wparam));
        }
        return 0;
    case WM_HSCROLL:
        if (application != nullptr) {
            application->on_horizontal_scroll(reinterpret_cast<HWND>(lparam));
        }
        return 0;
    case WM_TIMER:
        if (application != nullptr && wparam == timer_resources) {
            application->update_resources();
        } else if (application != nullptr && wparam == timer_meters) {
            application->update_meters();
        }
        return 0;
    case WM_DRAWITEM:
        if (application != nullptr) {
            application->draw_button(*reinterpret_cast<DRAWITEMSTRUCT*>(lparam));
            return TRUE;
        }
        break;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
        if (application != nullptr) {
            return reinterpret_cast<LRESULT>(application->brush_for_control(
                reinterpret_cast<HWND>(lparam), reinterpret_cast<HDC>(wparam)));
        }
        break;
    case message_engine_loaded:
        if (application != nullptr) {
            application->on_engine_loaded(wparam != FALSE);
        }
        return 0;
    case message_recognition_event:
        if (application != nullptr) {
            application->on_recognition_event(
                reinterpret_cast<RecognitionEvent*>(lparam));
        } else {
            delete reinterpret_cast<RecognitionEvent*>(lparam);
        }
        return 0;
    case message_session_stopped:
        if (application != nullptr) {
            application->on_session_stopped();
        }
        return 0;
    case WM_ERASEBKGND: {
        RECT area{};
        GetClientRect(window, &area);
        HBRUSH brush = CreateSolidBrush(color_window);
        FillRect(reinterpret_cast<HDC>(wparam), &area, brush);
        DeleteObject(brush);
        return TRUE;
    }
    case WM_DESTROY:
        KillTimer(window, timer_resources);
        KillTimer(window, timer_meters);
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        delete application;
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int command_show) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_BAR_CLASSES};
    InitCommonControlsEx(&controls);

    const wchar_t* class_name = L"SenseVoiceLocalDictationWindow";
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.style = CS_HREDRAW | CS_VREDRAW;
    window_class.lpfnWndProc = window_procedure;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    window_class.hIconSm = window_class.hIcon;
    window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    window_class.lpszClassName = class_name;
    if (RegisterClassExW(&window_class) == 0) {
        return 1;
    }

    RECT work_area{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work_area, 0);
    const UINT dpi = GetDpiForSystem();
    const int work_width = work_area.right - work_area.left;
    const int work_height = work_area.bottom - work_area.top;
    const int window_width = std::min(
        MulDiv(900, static_cast<int>(dpi), 96),
        work_width - MulDiv(32, static_cast<int>(dpi), 96));
    const int window_height = std::min(
        MulDiv(520, static_cast<int>(dpi), 96),
        work_height - MulDiv(32, static_cast<int>(dpi), 96));
    const int window_left = work_area.left + (work_width - window_width) / 2;
    const int window_top = work_area.top + (work_height - window_height) / 2;
    HWND window = CreateWindowExW(
        0,
        class_name,
        L"SenseVoice 本地听写",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        window_left,
        window_top,
        window_width,
        window_height,
        nullptr,
        nullptr,
        instance,
        nullptr);
    if (window == nullptr) {
        return 1;
    }
    ShowWindow(window, command_show);
    UpdateWindow(window);

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}
