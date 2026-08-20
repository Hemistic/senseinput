#include "windows_text_injector.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <ole2.h>
#include <UIAutomationClient.h>
#include <msctf.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <climits>
#include <cwchar>
#include <iterator>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using Microsoft::WRL::ComPtr;

struct WindowsTextInputTargetState {
    TF_INPUTPROCESSORPROFILE saved_profile{};
    bool has_saved_profile = false;
    bool profile_changed = false;
    bool tsf_ready = false;
    std::atomic<bool> restored = false;

    ~WindowsTextInputTargetState();
    void restore();
};

namespace {

inline constexpr CLSID openless_text_service_clsid = {
    0x6b9f3f4f,
    0x5ee7,
    0x42d6,
    {0x9c, 0x61, 0x9f, 0x80, 0xb0, 0x3a, 0x5d, 0x7d},
};
inline constexpr GUID openless_profile_guid = {
    0x9b5f5e04,
    0x23f6,
    0x47da,
    {0x9a, 0x26, 0xd2, 0x21, 0xf6, 0xc3, 0xf0, 0x2e},
};
constexpr LANGID openless_language_id = 0x0804;
constexpr wchar_t ime_pipe_prefix[] = L"\\\\.\\pipe\\OpenLessImeSubmit";
constexpr DWORD profile_activation_flags =
    TF_IPPMF_FORSESSION | TF_IPPMF_DONTCARECURRENTINPUTLANGUAGE | TF_IPPMF_ENABLEPROFILE;
constexpr DWORD profile_restore_flags =
    TF_IPPMF_FORSESSION | TF_IPPMF_DONTCARECURRENTINPUTLANGUAGE;

class ComApartment final {
public:
    ComApartment() : result_(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)) {}
    ComApartment(const ComApartment&) = delete;
    ComApartment& operator=(const ComApartment&) = delete;

    ~ComApartment() {
        if (result_ == S_OK || result_ == S_FALSE) CoUninitialize();
    }

    [[nodiscard]] bool usable() const {
        return SUCCEEDED(result_) || result_ == RPC_E_CHANGED_MODE;
    }

private:
    HRESULT result_ = E_FAIL;
};

class UniqueHandle final {
public:
    explicit UniqueHandle(HANDLE value = INVALID_HANDLE_VALUE) : value_(value) {}
    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    ~UniqueHandle() {
        if (value_ != nullptr && value_ != INVALID_HANDLE_VALUE) CloseHandle(value_);
    }

    [[nodiscard]] HANDLE get() const { return value_; }
    [[nodiscard]] bool valid() const {
        return value_ != nullptr && value_ != INVALID_HANDLE_VALUE;
    }

private:
    HANDLE value_ = INVALID_HANDLE_VALUE;
};

bool create_profile_manager(ComPtr<ITfInputProcessorProfileMgr>& manager) {
    return SUCCEEDED(CoCreateInstance(
        CLSID_TF_InputProcessorProfiles,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(manager.ReleaseAndGetAddressOf())));
}

bool create_profiles(ComPtr<ITfInputProcessorProfiles>& profiles) {
    return SUCCEEDED(CoCreateInstance(
        CLSID_TF_InputProcessorProfiles,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(profiles.ReleaseAndGetAddressOf())));
}

bool is_openless_profile(const TF_INPUTPROCESSORPROFILE& profile) {
    return profile.dwProfileType == TF_PROFILETYPE_INPUTPROCESSOR &&
        profile.langid == openless_language_id &&
        IsEqualGUID(profile.clsid, openless_text_service_clsid) != FALSE &&
        IsEqualGUID(profile.guidProfile, openless_profile_guid) != FALSE;
}

void restore_input_profile(const TF_INPUTPROCESSORPROFILE& profile) {
    if (is_openless_profile(profile)) return;

    ComApartment apartment;
    if (!apartment.usable()) return;

    ComPtr<ITfInputProcessorProfiles> profiles;
    if (create_profiles(profiles)) {
        if (SUCCEEDED(profiles->ChangeCurrentLanguage(profile.langid)) &&
            profile.dwProfileType == TF_PROFILETYPE_INPUTPROCESSOR) {
            profiles->ActivateLanguageProfile(
                profile.clsid,
                profile.langid,
                profile.guidProfile);
        }
    }

    ComPtr<ITfInputProcessorProfileMgr> manager;
    if (create_profile_manager(manager)) {
        manager->ActivateProfile(
            profile.dwProfileType,
            profile.langid,
            profile.clsid,
            profile.guidProfile,
            profile.hkl,
            profile_restore_flags);
    }
}

std::shared_ptr<WindowsTextInputTargetState> prepare_tsf_session() {
    ComApartment apartment;
    if (!apartment.usable()) return {};

    ComPtr<ITfInputProcessorProfileMgr> manager;
    if (!create_profile_manager(manager)) return {};

    auto state = std::make_shared<WindowsTextInputTargetState>();
    if (FAILED(manager->GetActiveProfile(
            GUID_TFCAT_TIP_KEYBOARD,
            &state->saved_profile))) {
        return {};
    }
    state->has_saved_profile = true;

    ComPtr<ITfInputProcessorProfiles> profiles;
    HRESULT legacy_result = E_FAIL;
    if (create_profiles(profiles)) {
        legacy_result = profiles->EnableLanguageProfile(
            openless_text_service_clsid,
            openless_language_id,
            openless_profile_guid,
            TRUE);
        if (SUCCEEDED(legacy_result)) {
            legacy_result = profiles->ChangeCurrentLanguage(openless_language_id);
        }
        if (SUCCEEDED(legacy_result)) {
            legacy_result = profiles->ActivateLanguageProfile(
                openless_text_service_clsid,
                openless_language_id,
                openless_profile_guid);
        }
    }

    const HRESULT modern_result = manager->ActivateProfile(
        TF_PROFILETYPE_INPUTPROCESSOR,
        openless_language_id,
        openless_text_service_clsid,
        openless_profile_guid,
        nullptr,
        profile_activation_flags);
    state->profile_changed = SUCCEEDED(legacy_result) || SUCCEEDED(modern_result);
    state->tsf_ready = SUCCEEDED(legacy_result) && SUCCEEDED(modern_result);
    return state;
}

bool is_native_edit_control(HWND window) {
    wchar_t class_name[64]{};
    if (GetClassNameW(window, class_name, static_cast<int>(std::size(class_name))) == 0) {
        return false;
    }
    return _wcsicmp(class_name, L"Edit") == 0 || _wcsnicmp(class_name, L"RichEdit", 8) == 0;
}

bool inject_native_edit(HWND focus, std::wstring_view text) {
    if (focus == nullptr || IsWindow(focus) == FALSE || IsWindowEnabled(focus) == FALSE) {
        return false;
    }
    if (!is_native_edit_control(focus) ||
        (GetWindowLongPtrW(focus, GWL_STYLE) & ES_READONLY) != 0) {
        return false;
    }

    const std::wstring owned_text(text);
    DWORD_PTR result = 0;
    return SendMessageTimeoutW(
               focus,
               EM_REPLACESEL,
               TRUE,
               reinterpret_cast<LPARAM>(owned_text.c_str()),
               SMTO_ABORTIFHUNG | SMTO_BLOCK,
               500,
               &result) != 0;
}

std::wstring escape_json(std::wstring_view text) {
    constexpr wchar_t hex[] = L"0123456789ABCDEF";
    std::wstring escaped;
    escaped.reserve(text.size() + 16);
    for (const wchar_t character : text) {
        switch (character) {
            case L'\"': escaped += L"\\\""; break;
            case L'\\': escaped += L"\\\\"; break;
            case L'\b': escaped += L"\\b"; break;
            case L'\f': escaped += L"\\f"; break;
            case L'\n': escaped += L"\\n"; break;
            case L'\r': escaped += L"\\r"; break;
            case L'\t': escaped += L"\\t"; break;
            default:
                if (character < 0x20) {
                    escaped += L"\\u";
                    escaped.push_back(hex[(character >> 12) & 0x0f]);
                    escaped.push_back(hex[(character >> 8) & 0x0f]);
                    escaped.push_back(hex[(character >> 4) & 0x0f]);
                    escaped.push_back(hex[character & 0x0f]);
                } else {
                    escaped.push_back(character);
                }
                break;
        }
    }
    return escaped;
}

bool wide_to_utf8(std::wstring_view wide, std::string& utf8) {
    utf8.clear();
    if (wide.empty()) return true;
    if (wide.size() > static_cast<std::size_t>(INT_MAX)) return false;
    const int required = WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        wide.data(),
        static_cast<int>(wide.size()),
        nullptr,
        0,
        nullptr,
        nullptr);
    if (required <= 0) return false;
    utf8.resize(static_cast<std::size_t>(required));
    return WideCharToMultiByte(
               CP_UTF8,
               WC_ERR_INVALID_CHARS,
               wide.data(),
               static_cast<int>(wide.size()),
               utf8.data(),
               required,
               nullptr,
               nullptr) == required;
}

std::wstring pipe_name_for_target(DWORD process_id, DWORD thread_id) {
    std::wstring name = ime_pipe_prefix;
    name += L"-";
    name += std::to_wstring(process_id);
    name += L"-";
    name += std::to_wstring(thread_id);
    return name;
}

std::vector<std::wstring> ime_pipe_candidates(DWORD process_id, DWORD thread_id) {
    std::vector<std::wstring> candidates{pipe_name_for_target(process_id, thread_id)};
    const std::wstring process_prefix =
        std::wstring(ime_pipe_prefix) + L"-" + std::to_wstring(process_id) + L"-";
    const std::wstring pattern = process_prefix + L"*";
    WIN32_FIND_DATAW find_data{};
    HANDLE search = FindFirstFileW(pattern.c_str(), &find_data);
    if (search == INVALID_HANDLE_VALUE) return candidates;

    do {
        std::wstring candidate = L"\\\\.\\pipe\\";
        candidate += find_data.cFileName;
        if (candidate == candidates.front() || !candidate.starts_with(process_prefix)) continue;
        const std::wstring_view suffix(candidate.data() + process_prefix.size(),
                                       candidate.size() - process_prefix.size());
        if (!suffix.empty() && std::all_of(suffix.begin(), suffix.end(), [](wchar_t character) {
                return character >= L'0' && character <= L'9';
            })) {
            candidates.push_back(std::move(candidate));
        }
    } while (FindNextFileW(search, &find_data) != FALSE);
    FindClose(search);
    return candidates;
}

UniqueHandle open_ime_pipe(DWORD process_id, DWORD thread_id) {
    const ULONGLONG deadline = GetTickCount64() + 700;
    do {
        for (const std::wstring& pipe_name : ime_pipe_candidates(process_id, thread_id)) {
            HANDLE pipe = CreateFileW(
                pipe_name.c_str(),
                GENERIC_READ | GENERIC_WRITE,
                0,
                nullptr,
                OPEN_EXISTING,
                0,
                nullptr);
            if (pipe != INVALID_HANDLE_VALUE) return UniqueHandle(pipe);

            const DWORD error = GetLastError();
            if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PIPE_BUSY &&
                error != ERROR_SEM_TIMEOUT) {
                return UniqueHandle();
            }
            WaitNamedPipeW(pipe_name.c_str(), 25);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    } while (GetTickCount64() < deadline);
    return UniqueHandle();
}

enum class TsfSubmitResult {
    Unavailable,
    Committed,
    Rejected,
    Indeterminate,
};

TsfSubmitResult submit_through_tsf(
    const WindowsTextInputTarget& target,
    DWORD process_id,
    DWORD thread_id,
    std::wstring_view text) {
    if (!target.state || !target.state->tsf_ready) return TsfSubmitResult::Unavailable;

    UniqueHandle pipe = open_ime_pipe(process_id, thread_id);
    if (!pipe.valid()) return TsfSubmitResult::Unavailable;

    const std::wstring session_id =
        std::to_wstring(GetCurrentProcessId()) + L"-" +
        std::to_wstring(GetCurrentThreadId()) + L"-" +
        std::to_wstring(GetTickCount64());
    std::wstring request = L"{\"type\":\"submitText\",\"protocolVersion\":1,";
    request += L"\"sessionId\":\"" + session_id + L"\",\"text\":\"";
    request += escape_json(text);
    request += L"\"}\n";

    std::string request_utf8;
    if (!wide_to_utf8(request, request_utf8) || request_utf8.size() >= 64 * 1024) {
        return TsfSubmitResult::Unavailable;
    }

    DWORD written = 0;
    if (WriteFile(
            pipe.get(),
            request_utf8.data(),
            static_cast<DWORD>(request_utf8.size()),
            &written,
            nullptr) == FALSE) {
        return written == 0 ? TsfSubmitResult::Unavailable : TsfSubmitResult::Indeterminate;
    }
    if (written != request_utf8.size()) return TsfSubmitResult::Indeterminate;

    std::string response;
    std::array<char, 1024> buffer{};
    while (response.size() < 8192) {
        DWORD bytes_read = 0;
        if (ReadFile(
                pipe.get(),
                buffer.data(),
                static_cast<DWORD>(buffer.size()),
                &bytes_read,
                nullptr) == FALSE ||
            bytes_read == 0) {
            return TsfSubmitResult::Indeterminate;
        }
        response.append(buffer.data(), bytes_read);
        if (response.find('\n') != std::string::npos) break;
    }

    std::string session_id_utf8;
    if (!wide_to_utf8(session_id, session_id_utf8)) return TsfSubmitResult::Indeterminate;
    const std::string expected_session =
        "\"sessionId\":\"" + session_id_utf8 + "\"";
    if (response.find(expected_session) == std::string::npos) {
        return TsfSubmitResult::Indeterminate;
    }
    if (response.find("\"status\":\"committed\"") != std::string::npos) {
        return TsfSubmitResult::Committed;
    }
    if (response.find("\"status\":\"rejected\"") != std::string::npos ||
        response.find("\"status\":\"failed\"") != std::string::npos) {
        return TsfSubmitResult::Rejected;
    }
    return TsfSubmitResult::Indeterminate;
}

bool bstr_to_wstring(BSTR value, std::wstring& output) {
    if (value == nullptr) {
        output.clear();
        return true;
    }
    output.assign(value, SysStringLen(value));
    return true;
}

bool range_text(IUIAutomationTextRange* range, std::wstring& output) {
    BSTR value = nullptr;
    const HRESULT result = range->GetText(-1, &value);
    if (FAILED(result)) return false;
    bstr_to_wstring(value, output);
    SysFreeString(value);
    return true;
}

bool inject_with_uia(IUIAutomationElement* element, DWORD process_id, std::wstring_view text) {
    if (element == nullptr || text.size() > static_cast<std::size_t>(INT_MAX)) return false;

    int element_process_id = 0;
    BOOL enabled = FALSE;
    BOOL password = FALSE;
    if (FAILED(element->get_CurrentProcessId(&element_process_id)) ||
        static_cast<DWORD>(element_process_id) != process_id ||
        FAILED(element->get_CurrentIsEnabled(&enabled)) || enabled == FALSE ||
        FAILED(element->get_CurrentIsPassword(&password)) || password != FALSE) {
        return false;
    }

    ComPtr<IUIAutomationValuePattern> value_pattern;
    ComPtr<IUIAutomationTextPattern> text_pattern;
    if (FAILED(element->GetCurrentPatternAs(
            UIA_ValuePatternId,
            __uuidof(IUIAutomationValuePattern),
            reinterpret_cast<void**>(value_pattern.ReleaseAndGetAddressOf()))) ||
        FAILED(element->GetCurrentPatternAs(
            UIA_TextPatternId,
            __uuidof(IUIAutomationTextPattern),
            reinterpret_cast<void**>(text_pattern.ReleaseAndGetAddressOf())))) {
        return false;
    }

    BOOL read_only = TRUE;
    if (FAILED(value_pattern->get_CurrentIsReadOnly(&read_only)) || read_only != FALSE) {
        return false;
    }

    ComPtr<IUIAutomationTextRangeArray> selections;
    ComPtr<IUIAutomationTextRange> document;
    if (FAILED(text_pattern->GetSelection(selections.ReleaseAndGetAddressOf())) ||
        FAILED(text_pattern->get_DocumentRange(document.ReleaseAndGetAddressOf()))) {
        return false;
    }
    int selection_count = 0;
    if (FAILED(selections->get_Length(&selection_count)) || selection_count != 1) return false;

    ComPtr<IUIAutomationTextRange> selection;
    ComPtr<IUIAutomationTextRange> prefix_range;
    ComPtr<IUIAutomationTextRange> suffix_range;
    if (FAILED(selections->GetElement(0, selection.ReleaseAndGetAddressOf())) ||
        FAILED(document->Clone(prefix_range.ReleaseAndGetAddressOf())) ||
        FAILED(document->Clone(suffix_range.ReleaseAndGetAddressOf())) ||
        FAILED(prefix_range->MoveEndpointByRange(
            TextPatternRangeEndpoint_End,
            selection.Get(),
            TextPatternRangeEndpoint_Start)) ||
        FAILED(suffix_range->MoveEndpointByRange(
            TextPatternRangeEndpoint_Start,
            selection.Get(),
            TextPatternRangeEndpoint_End))) {
        return false;
    }

    std::wstring prefix;
    std::wstring suffix;
    if (!range_text(prefix_range.Get(), prefix) || !range_text(suffix_range.Get(), suffix)) {
        return false;
    }

    std::wstring replacement;
    replacement.reserve(prefix.size() + text.size() + suffix.size());
    replacement += prefix;
    replacement.append(text);
    replacement += suffix;
    if (replacement.size() > std::numeric_limits<UINT>::max()) return false;

    BSTR replacement_bstr = SysAllocStringLen(
        replacement.data(),
        static_cast<UINT>(replacement.size()));
    if (replacement_bstr == nullptr && !replacement.empty()) return false;
    element->SetFocus();
    const HRESULT set_result = value_pattern->SetValue(replacement_bstr);
    SysFreeString(replacement_bstr);
    if (FAILED(set_result)) return false;

    ComPtr<IUIAutomationTextPattern> updated_pattern;
    ComPtr<IUIAutomationTextRange> updated_document;
    ComPtr<IUIAutomationTextRange> caret;
    if (SUCCEEDED(element->GetCurrentPatternAs(
            UIA_TextPatternId,
            __uuidof(IUIAutomationTextPattern),
            reinterpret_cast<void**>(updated_pattern.ReleaseAndGetAddressOf()))) &&
        SUCCEEDED(updated_pattern->get_DocumentRange(updated_document.ReleaseAndGetAddressOf())) &&
        SUCCEEDED(updated_document->Clone(caret.ReleaseAndGetAddressOf())) &&
        SUCCEEDED(caret->MoveEndpointByRange(
            TextPatternRangeEndpoint_End,
            updated_document.Get(),
            TextPatternRangeEndpoint_Start))) {
        int moved = 0;
        const std::size_t caret_offset = prefix.size() + text.size();
        if (caret_offset <= static_cast<std::size_t>(INT_MAX) &&
            SUCCEEDED(caret->Move(TextUnit_Character, static_cast<int>(caret_offset), &moved)) &&
            moved == static_cast<int>(caret_offset)) {
            caret->Select();
        }
    }
    return true;
}

bool inject_accessible_control(HWND focus, DWORD process_id, std::wstring_view text) {
    ComApartment apartment;
    if (!apartment.usable()) return false;

    ComPtr<IUIAutomation> automation;
    if (FAILED(CoCreateInstance(
            CLSID_CUIAutomation,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(automation.ReleaseAndGetAddressOf())))) {
        return false;
    }

    ComPtr<IUIAutomationElement> element;
    if (SUCCEEDED(automation->GetFocusedElement(element.ReleaseAndGetAddressOf())) &&
        inject_with_uia(element.Get(), process_id, text)) {
        return true;
    }
    if (focus == nullptr) return false;
    element.Reset();
    return SUCCEEDED(automation->ElementFromHandle(
               reinterpret_cast<UIA_HWND>(focus),
               element.ReleaseAndGetAddressOf())) &&
        inject_with_uia(element.Get(), process_id, text);
}

} // namespace

WindowsTextInputTargetState::~WindowsTextInputTargetState() {
    restore();
}

void WindowsTextInputTargetState::restore() {
    if (!has_saved_profile || !profile_changed || restored.exchange(true)) return;
    restore_input_profile(saved_profile);
}

WindowsTextInputTarget capture_windows_text_input_target(std::uint32_t excluded_process_id) {
    HWND window = GetForegroundWindow();
    if (window == nullptr) return {};

    DWORD process_id = 0;
    const DWORD thread_id = GetWindowThreadProcessId(window, &process_id);
    if (process_id == excluded_process_id) return {};

    window = GetAncestor(window, GA_ROOT);
    if (window == nullptr || IsWindow(window) == FALSE) return {};

    GUITHREADINFO gui_info{sizeof(gui_info)};
    HWND focus = nullptr;
    if (thread_id != 0 && GetGUIThreadInfo(thread_id, &gui_info) != FALSE) {
        focus = gui_info.hwndFocus;
    }
    return {
        .window = reinterpret_cast<std::uintptr_t>(window),
        .focus = reinterpret_cast<std::uintptr_t>(focus),
        .state = prepare_tsf_session(),
    };
}

bool windows_text_input_target_has_tsf_session(const WindowsTextInputTarget& target) {
    return target.state && target.state->tsf_ready;
}

bool inject_text_into_windows_text_input(
    WindowsTextInputTarget target_value,
    std::wstring_view text,
    std::uint32_t excluded_process_id) {
    if (text.empty()) return false;
    HWND target = reinterpret_cast<HWND>(target_value.window);
    if (target == nullptr || IsWindow(target) == FALSE) return false;
    target = GetAncestor(target, GA_ROOT);
    if (target == nullptr || IsWindow(target) == FALSE) return false;

    DWORD process_id = 0;
    DWORD target_thread_id = GetWindowThreadProcessId(target, &process_id);
    if (process_id == excluded_process_id || target_thread_id == 0) return false;

    HWND focus = reinterpret_cast<HWND>(target_value.focus);
    DWORD focus_process_id = 0;
    const DWORD focus_thread_id = focus != nullptr && IsWindow(focus) != FALSE
        ? GetWindowThreadProcessId(focus, &focus_process_id)
        : 0;
    if (focus_thread_id != 0 && focus_process_id == process_id) {
        target_thread_id = focus_thread_id;
    } else {
        focus = nullptr;
    }

    const TsfSubmitResult tsf_result = submit_through_tsf(
        target_value,
        process_id,
        target_thread_id,
        text);
    if (target_value.state) target_value.state->restore();
    if (tsf_result == TsfSubmitResult::Committed) return true;
    if (tsf_result == TsfSubmitResult::Indeterminate) return false;

    if (focus != nullptr && inject_native_edit(focus, text)) return true;

    if (IsIconic(target) != FALSE) ShowWindow(target, SW_RESTORE);
    const DWORD current_thread_id = GetCurrentThreadId();
    const bool attached = target_thread_id != current_thread_id &&
        AttachThreadInput(current_thread_id, target_thread_id, TRUE) != FALSE;

    BringWindowToTop(target);
    SetActiveWindow(target);
    SetForegroundWindow(target);
    for (int attempt = 0; attempt < 3 && GetForegroundWindow() != target; ++attempt) {
        SetForegroundWindow(target);
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }

    bool injected = false;
    if (GetForegroundWindow() == target) {
        if (focus != nullptr) SetFocus(focus);
        injected = inject_accessible_control(focus, process_id, text);
    }
    if (attached) AttachThreadInput(current_thread_id, target_thread_id, FALSE);
    return injected;
}
