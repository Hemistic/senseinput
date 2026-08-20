#include "windows_text_injector.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <msctf.h>
#include <ole2.h>
#include <UIAutomationClient.h>
#include <wrl/client.h>

#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

constexpr wchar_t tsf_host_title[] = L"SenseVoice TSF injection integration host";
constexpr wchar_t tsf_first_text[] = L"第一次注入";
constexpr wchar_t tsf_second_text[] = L"，第二次继续";
constexpr wchar_t tsf_expected[] = L"开头第一次注入，第二次继续结尾";

int run_tsf_host() {
    const HRESULT com_result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    ITfThreadMgr* thread_manager = nullptr;
    TfClientId client_id = TF_CLIENTID_NULL;
    if (FAILED(CoCreateInstance(
            CLSID_TF_ThreadMgr,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_ITfThreadMgr,
            reinterpret_cast<void**>(&thread_manager))) ||
        FAILED(thread_manager->Activate(&client_id))) {
        if (thread_manager != nullptr) thread_manager->Release();
        if (com_result == S_OK || com_result == S_FALSE) CoUninitialize();
        return 1;
    }

    HWND parent = CreateWindowExW(
        0, L"STATIC", tsf_host_title, WS_OVERLAPPED | WS_VISIBLE,
        100, 100, 360, 120, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    HWND edit = CreateWindowExW(
        0, L"EDIT", L"开头待替换结尾", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        10, 10, 320, 40, parent, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (parent == nullptr || edit == nullptr) {
        thread_manager->Deactivate();
        thread_manager->Release();
        if (com_result == S_OK || com_result == S_FALSE) CoUninitialize();
        return 1;
    }

    ShowWindow(parent, SW_SHOW);
    SetForegroundWindow(parent);
    SetFocus(edit);
    SendMessageW(edit, EM_SETSEL, 2, 5);

    const ULONGLONG deadline = GetTickCount64() + 8000;
    while (GetTickCount64() < deadline && IsWindow(parent) != FALSE) {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != FALSE) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        wchar_t actual[128]{};
        GetWindowTextW(edit, actual, static_cast<int>(std::size(actual)));
        if (std::wstring(actual) == tsf_expected) {
            DestroyWindow(parent);
            thread_manager->Deactivate();
            thread_manager->Release();
            if (com_result == S_OK || com_result == S_FALSE) CoUninitialize();
            return 0;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (IsWindow(parent) != FALSE) DestroyWindow(parent);
    thread_manager->Deactivate();
    thread_manager->Release();
    if (com_result == S_OK || com_result == S_FALSE) CoUninitialize();
    return 1;
}

bool wait_for_tsf_pipe(DWORD process_id, DWORD thread_id) {
    const std::wstring exact_pipe_name =
        L"\\\\.\\pipe\\OpenLessImeSubmit-" + std::to_wstring(process_id) + L"-" +
        std::to_wstring(thread_id);
    const std::wstring process_pattern =
        L"\\\\.\\pipe\\OpenLessImeSubmit-" + std::to_wstring(process_id) + L"-*";
    const ULONGLONG deadline = GetTickCount64() + 6000;
    do {
        if (WaitNamedPipeW(exact_pipe_name.c_str(), 50) != FALSE) return true;
        WIN32_FIND_DATAW find_data{};
        HANDLE search = FindFirstFileW(process_pattern.c_str(), &find_data);
        if (search != INVALID_HANDLE_VALUE) {
            FindClose(search);
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    } while (GetTickCount64() < deadline);
    return false;
}

void print_tsf_pipes() {
    WIN32_FIND_DATAW find_data{};
    HANDLE search = FindFirstFileW(L"\\\\.\\pipe\\OpenLessImeSubmit-*", &find_data);
    if (search == INVALID_HANDLE_VALUE) {
        std::cerr << "no OpenLess TSF pipes were visible\n";
        return;
    }
    do {
        std::wcerr << L"visible TSF pipe: " << find_data.cFileName << L"\n";
    } while (FindNextFileW(search, &find_data) != FALSE);
    FindClose(search);
}

struct ProcessWindowSearch {
    DWORD process_id = 0;
    HWND window = nullptr;
};

BOOL CALLBACK find_process_window_callback(HWND window, LPARAM parameter) {
    auto* search = reinterpret_cast<ProcessWindowSearch*>(parameter);
    DWORD process_id = 0;
    GetWindowThreadProcessId(window, &process_id);
    if (process_id != search->process_id ||
        IsWindowVisible(window) == FALSE ||
        GetWindow(window, GW_OWNER) != nullptr) {
        return TRUE;
    }
    search->window = window;
    return FALSE;
}

HWND find_top_level_window_for_process(DWORD process_id) {
    ProcessWindowSearch search{.process_id = process_id};
    EnumWindows(find_process_window_callback, reinterpret_cast<LPARAM>(&search));
    return search.window;
}

bool find_process_document(
    DWORD process_id,
    HWND window,
    ComPtr<IUIAutomation>& automation,
    ComPtr<IUIAutomationElement>& document) {
    if (FAILED(CoCreateInstance(
            CLSID_CUIAutomation,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(automation.ReleaseAndGetAddressOf())))) {
        return false;
    }

    ComPtr<IUIAutomationElement> root;
    if (FAILED(automation->ElementFromHandle(
            reinterpret_cast<UIA_HWND>(window),
            root.ReleaseAndGetAddressOf()))) {
        return false;
    }

    VARIANT value{};
    VariantInit(&value);
    value.vt = VT_I4;
    value.lVal = UIA_DocumentControlTypeId;
    ComPtr<IUIAutomationCondition> document_condition;
    const HRESULT condition_result = automation->CreatePropertyCondition(
        UIA_ControlTypePropertyId,
        value,
        document_condition.ReleaseAndGetAddressOf());
    VariantClear(&value);
    if (FAILED(condition_result) ||
        FAILED(root->FindFirst(
            TreeScope_Subtree,
            document_condition.Get(),
            document.ReleaseAndGetAddressOf())) ||
        !document) {
        return false;
    }

    int document_process_id = 0;
    return SUCCEEDED(document->get_CurrentProcessId(&document_process_id)) &&
        static_cast<DWORD>(document_process_id) == process_id;
}

bool select_notepad_test_range(DWORD process_id, HWND window) {
    ComPtr<IUIAutomation> automation;
    ComPtr<IUIAutomationElement> document;
    if (!find_process_document(process_id, window, automation, document)) return false;
    return SUCCEEDED(document->SetFocus());
}

bool read_notepad_text(DWORD process_id, HWND window, std::wstring& text) {
    ComPtr<IUIAutomation> automation;
    ComPtr<IUIAutomationElement> document;
    if (!find_process_document(process_id, window, automation, document)) return false;

    ComPtr<IUIAutomationTextPattern> pattern;
    ComPtr<IUIAutomationTextRange> range;
    BSTR value = nullptr;
    if (FAILED(document->GetCurrentPatternAs(
            UIA_TextPatternId,
            __uuidof(IUIAutomationTextPattern),
            reinterpret_cast<void**>(pattern.ReleaseAndGetAddressOf()))) ||
        FAILED(pattern->get_DocumentRange(range.ReleaseAndGetAddressOf())) ||
        FAILED(range->GetText(-1, &value))) {
        return false;
    }
    text.assign(value != nullptr ? value : L"", value != nullptr ? SysStringLen(value) : 0);
    SysFreeString(value);
    return true;
}

bool write_notepad_fixture(const std::filesystem::path& path) {
    const std::wstring contents = L"\ufeff";
    HANDLE file = CreateFileW(
        path.c_str(),
        GENERIC_WRITE,
        0,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_TEMPORARY,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const DWORD byte_count = static_cast<DWORD>(contents.size() * sizeof(wchar_t));
    const bool success = WriteFile(file, contents.data(), byte_count, &written, nullptr) != FALSE &&
        written == byte_count;
    CloseHandle(file);
    return success;
}

int run_tsf_integration() {
    const char* failure_stage = "launch";
    wchar_t executable_path[MAX_PATH]{};
    if (GetModuleFileNameW(nullptr, executable_path, static_cast<DWORD>(std::size(executable_path))) == 0) {
        return 1;
    }
    std::wstring command_line = L"\"" + std::wstring(executable_path) + L"\" --tsf-host";
    std::vector<wchar_t> writable_command(command_line.begin(), command_line.end());
    writable_command.push_back(L'\0');

    STARTUPINFOW startup_info{sizeof(startup_info)};
    PROCESS_INFORMATION process_info{};
    if (CreateProcessW(
            executable_path,
            writable_command.data(),
            nullptr,
            nullptr,
            FALSE,
            0,
            nullptr,
            nullptr,
            &startup_info,
            &process_info) == FALSE) {
        return 1;
    }
    CloseHandle(process_info.hThread);

    HWND host = nullptr;
    const ULONGLONG window_deadline = GetTickCount64() + 3000;
    do {
        host = FindWindowW(L"Static", tsf_host_title);
        if (host != nullptr) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    } while (GetTickCount64() < window_deadline);

    bool success = host != nullptr;
    if (!success) failure_stage = "find-host-window";
    if (success) {
        SetForegroundWindow(host);
        WindowsTextInputTarget first_target = capture_windows_text_input_target(GetCurrentProcessId());
        DWORD host_process_id = 0;
        const DWORD host_thread_id = GetWindowThreadProcessId(host, &host_process_id);
        success = first_target.valid() && host_process_id == process_info.dwProcessId;
        if (!success) failure_stage = "capture-first-target";
        if (success && !windows_text_input_target_has_tsf_session(first_target)) {
            success = false;
            failure_stage = "activate-first-profile";
        }
        if (success) {
            success = wait_for_tsf_pipe(host_process_id, host_thread_id);
            if (!success) failure_stage = "wait-first-pipe";
        }
        first_target.focus = 0;
        if (success) {
            success = inject_text_into_windows_text_input(
                std::move(first_target), tsf_first_text, GetCurrentProcessId());
            if (!success) failure_stage = "submit-first-text";
        }

        if (success) {
            SetForegroundWindow(host);
            WindowsTextInputTarget second_target =
                capture_windows_text_input_target(GetCurrentProcessId());
            success = second_target.valid();
            if (!success) failure_stage = "capture-second-target";
            if (success && !windows_text_input_target_has_tsf_session(second_target)) {
                success = false;
                failure_stage = "activate-second-profile";
            }
            if (success) {
                success = wait_for_tsf_pipe(host_process_id, host_thread_id);
                if (!success) failure_stage = "wait-second-pipe";
            }
            second_target.focus = 0;
            if (success) {
                success = inject_text_into_windows_text_input(
                    std::move(second_target), tsf_second_text, GetCurrentProcessId());
                if (!success) failure_stage = "submit-second-text";
            }
        }
    }

    const DWORD wait_result = WaitForSingleObject(process_info.hProcess, 9000);
    DWORD exit_code = 1;
    if (wait_result == WAIT_OBJECT_0) {
        GetExitCodeProcess(process_info.hProcess, &exit_code);
    } else if (host != nullptr) {
        PostMessageW(host, WM_CLOSE, 0, 0);
    }
    CloseHandle(process_info.hProcess);

    if (!success || wait_result != WAIT_OBJECT_0 || exit_code != 0) {
        std::cerr << "TSF cross-process consecutive insertion failed at " << failure_stage
                  << ", host-exit=" << exit_code << "\n";
        print_tsf_pipes();
        return 1;
    }
    return 0;
}

int run_qt_tsf_integration() {
    const char* failure_stage = "launch";
    wchar_t executable_path[MAX_PATH]{};
    if (GetModuleFileNameW(
            nullptr, executable_path, static_cast<DWORD>(std::size(executable_path))) == 0) {
        return 1;
    }
    std::filesystem::path target_path(executable_path);
    target_path.replace_filename(L"windows-text-target.exe");
    const std::filesystem::path runtime_directory =
        target_path.parent_path().parent_path().parent_path() / L"dist";
    std::wstring command_line = L"\"" + target_path.wstring() + L"\"";
    std::vector<wchar_t> writable_command(command_line.begin(), command_line.end());
    writable_command.push_back(L'\0');

    STARTUPINFOW startup_info{sizeof(startup_info)};
    PROCESS_INFORMATION process_info{};
    if (CreateProcessW(
            target_path.c_str(),
            writable_command.data(),
            nullptr,
            nullptr,
            FALSE,
            0,
            nullptr,
            runtime_directory.c_str(),
            &startup_info,
            &process_info) == FALSE) {
        std::cerr << "failed to launch Qt target: " << GetLastError() << "\n";
        return 1;
    }
    CloseHandle(process_info.hThread);

    HWND host = nullptr;
    const ULONGLONG window_deadline = GetTickCount64() + 4000;
    do {
        host = find_top_level_window_for_process(process_info.dwProcessId);
        if (host != nullptr) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    } while (GetTickCount64() < window_deadline);

    bool success = host != nullptr;
    if (!success) failure_stage = "find-qt-host-window";
    for (int injection = 0; success && injection < 2; ++injection) {
        SetForegroundWindow(host);
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        WindowsTextInputTarget target =
            capture_windows_text_input_target(GetCurrentProcessId());
        DWORD host_process_id = 0;
        const DWORD host_thread_id = GetWindowThreadProcessId(host, &host_process_id);
        success = target.valid() && host_process_id == process_info.dwProcessId;
        if (!success) {
            failure_stage = injection == 0 ? "capture-first-qt-target" : "capture-second-qt-target";
            break;
        }
        if (!windows_text_input_target_has_tsf_session(target)) {
            success = false;
            failure_stage = injection == 0 ? "activate-first-qt-profile" : "activate-second-qt-profile";
            break;
        }
        if (!wait_for_tsf_pipe(host_process_id, host_thread_id)) {
            success = false;
            failure_stage = injection == 0 ? "wait-first-qt-pipe" : "wait-second-qt-pipe";
            break;
        }
        target.focus = 0;
        const std::wstring_view text = injection == 0 ? tsf_first_text : tsf_second_text;
        if (!inject_text_into_windows_text_input(
                std::move(target), text, GetCurrentProcessId())) {
            success = false;
            failure_stage = injection == 0 ? "submit-first-qt-text" : "submit-second-qt-text";
        }
    }

    const DWORD wait_result = WaitForSingleObject(process_info.hProcess, 10'500);
    DWORD exit_code = 1;
    if (wait_result == WAIT_OBJECT_0) {
        GetExitCodeProcess(process_info.hProcess, &exit_code);
    } else if (host != nullptr) {
        PostMessageW(host, WM_CLOSE, 0, 0);
    }
    CloseHandle(process_info.hProcess);

    if (!success || wait_result != WAIT_OBJECT_0 || exit_code != 0) {
        std::cerr << "Qt TSF consecutive insertion failed at " << failure_stage
                  << ", host-exit=" << exit_code << "\n";
        print_tsf_pipes();
        return 1;
    }
    return 0;
}

int run_notepad_tsf_integration() {
    constexpr std::wstring_view expected = L"第一次注入，第二次继续";
    const HRESULT com_result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool should_uninitialize = com_result == S_OK || com_result == S_FALSE;
    if (FAILED(com_result) && com_result != RPC_E_CHANGED_MODE) return 1;

    wchar_t temp_directory[MAX_PATH]{};
    if (GetTempPathW(static_cast<DWORD>(std::size(temp_directory)), temp_directory) == 0) {
        if (should_uninitialize) CoUninitialize();
        return 1;
    }
    const std::filesystem::path fixture =
        std::filesystem::path(temp_directory) /
        (L"sensevoice-tsf-" + std::to_wstring(GetCurrentProcessId()) + L".txt");
    if (!write_notepad_fixture(fixture)) {
        if (should_uninitialize) CoUninitialize();
        return 1;
    }

    wchar_t system_directory[MAX_PATH]{};
    GetSystemDirectoryW(system_directory, static_cast<UINT>(std::size(system_directory)));
    const std::filesystem::path notepad_path =
        std::filesystem::path(system_directory) / L"notepad.exe";
    std::wstring command_line =
        L"\"" + notepad_path.wstring() + L"\" /newWindow \"" + fixture.wstring() + L"\"";
    std::vector<wchar_t> writable_command(command_line.begin(), command_line.end());
    writable_command.push_back(L'\0');

    STARTUPINFOW startup_info{sizeof(startup_info)};
    PROCESS_INFORMATION process_info{};
    if (CreateProcessW(
            notepad_path.c_str(),
            writable_command.data(),
            nullptr,
            nullptr,
            FALSE,
            0,
            nullptr,
            nullptr,
            &startup_info,
            &process_info) == FALSE) {
        DeleteFileW(fixture.c_str());
        if (should_uninitialize) CoUninitialize();
        return 1;
    }
    CloseHandle(process_info.hThread);

    const auto cleanup = [&] {
        TerminateProcess(process_info.hProcess, 0);
        WaitForSingleObject(process_info.hProcess, 3000);
        CloseHandle(process_info.hProcess);
        DeleteFileW(fixture.c_str());
        if (should_uninitialize) CoUninitialize();
    };

    const char* failure_stage = "find-notepad-window";
    HWND host = nullptr;
    const ULONGLONG window_deadline = GetTickCount64() + 6000;
    do {
        host = find_top_level_window_for_process(process_info.dwProcessId);
        if (host != nullptr) break;
        if (WaitForSingleObject(process_info.hProcess, 0) == WAIT_OBJECT_0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    } while (GetTickCount64() < window_deadline);

    bool success = host != nullptr;
    if (success) {
        AllowSetForegroundWindow(process_info.dwProcessId);
        SetForegroundWindow(host);
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
    }

    for (int injection = 0; success && injection < 2; ++injection) {
        SetForegroundWindow(host);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        WindowsTextInputTarget target =
            capture_windows_text_input_target(GetCurrentProcessId());
        DWORD host_process_id = 0;
        const DWORD host_thread_id = GetWindowThreadProcessId(host, &host_process_id);
        if (injection == 0) {
            char class_name[128]{};
            GetClassNameA(host, class_name, static_cast<int>(std::size(class_name)));
            GUITHREADINFO gui_info{sizeof(gui_info)};
            GetGUIThreadInfo(host_thread_id, &gui_info);
            char focus_class[128]{};
            if (gui_info.hwndFocus != nullptr) {
                GetClassNameA(
                    gui_info.hwndFocus,
                    focus_class,
                    static_cast<int>(std::size(focus_class)));
            }
            std::cerr << "notepad-target pid=" << host_process_id
                      << " tid=" << host_thread_id
                      << " title-length=" << GetWindowTextLengthW(host)
                      << " class='" << class_name
                      << "' focus=" << reinterpret_cast<std::uintptr_t>(gui_info.hwndFocus)
                      << " focus-class='" << focus_class << "'\n";
        }
        success = target.valid() && host_process_id == process_info.dwProcessId;
        if (!success) {
            failure_stage = injection == 0
                ? "capture-first-notepad-target"
                : "capture-second-notepad-target";
            break;
        }
        if (!windows_text_input_target_has_tsf_session(target)) {
            success = false;
            failure_stage = injection == 0
                ? "activate-first-notepad-profile"
                : "activate-second-notepad-profile";
            break;
        }
        if (!wait_for_tsf_pipe(host_process_id, host_thread_id)) {
            success = false;
            failure_stage = injection == 0
                ? "wait-first-notepad-pipe"
                : "wait-second-notepad-pipe";
            break;
        }
        target.focus = 0;
        const std::wstring_view text = injection == 0 ? tsf_first_text : tsf_second_text;
        if (!inject_text_into_windows_text_input(
                std::move(target), text, GetCurrentProcessId())) {
            success = false;
            failure_stage = injection == 0
                ? "submit-first-notepad-text"
                : "submit-second-notepad-text";
        }
    }

    std::wstring actual;
    if (success) {
        success = read_notepad_text(process_info.dwProcessId, host, actual) &&
            actual.find(expected) != std::wstring::npos;
        if (!success) failure_stage = "verify-notepad-text";
    }

    if (!success) {
        std::cerr << "Notepad TSF consecutive insertion failed at " << failure_stage << "\n";
        print_tsf_pipes();
    }
    cleanup();
    return success ? 0 : 1;
}

int run_foreground_integration() {
    constexpr auto first_text = L"SENSEVOICE_REAL_TARGET_1";
    constexpr auto second_text = L"_SENSEVOICE_REAL_TARGET_2";

    std::cout << "waiting-for-foreground\n";
    std::this_thread::sleep_for(std::chrono::seconds(3));

    WindowsTextInputTarget first_target =
        capture_windows_text_input_target(GetCurrentProcessId());
    if (!first_target.valid()) {
        std::cerr << "capture-first-target failed\n";
        return 1;
    }

    const HWND first_window = reinterpret_cast<HWND>(first_target.window);
    DWORD first_process_id = 0;
    const DWORD first_thread_id =
        GetWindowThreadProcessId(first_window, &first_process_id);
    const bool first_tsf_ready = windows_text_input_target_has_tsf_session(first_target);
    const bool first_pipe_ready = first_tsf_ready &&
        wait_for_tsf_pipe(first_process_id, first_thread_id);
    std::cout << "first-target pid=" << first_process_id
              << " tid=" << first_thread_id
              << " tsf-ready=" << first_tsf_ready
              << " pipe-ready=" << first_pipe_ready << "\n";

    if (!inject_text_into_windows_text_input(
            std::move(first_target), first_text, GetCurrentProcessId())) {
        std::cerr << "submit-first-text failed\n";
        print_tsf_pipes();
        return 1;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    WindowsTextInputTarget second_target =
        capture_windows_text_input_target(GetCurrentProcessId());
    if (!second_target.valid()) {
        std::cerr << "capture-second-target failed\n";
        return 1;
    }

    const HWND second_window = reinterpret_cast<HWND>(second_target.window);
    DWORD second_process_id = 0;
    const DWORD second_thread_id =
        GetWindowThreadProcessId(second_window, &second_process_id);
    const bool second_tsf_ready = windows_text_input_target_has_tsf_session(second_target);
    const bool second_pipe_ready = second_tsf_ready &&
        wait_for_tsf_pipe(second_process_id, second_thread_id);
    std::cout << "second-target pid=" << second_process_id
              << " tid=" << second_thread_id
              << " tsf-ready=" << second_tsf_ready
              << " pipe-ready=" << second_pipe_ready << "\n";

    if (first_window != second_window ||
        !inject_text_into_windows_text_input(
            std::move(second_target), second_text, GetCurrentProcessId())) {
        std::cerr << "submit-second-text failed\n";
        print_tsf_pipes();
        return 1;
    }
    return 0;
}

} // namespace

int wmain(int argument_count, wchar_t* arguments[]) {
    if (argument_count == 2 && std::wstring_view(arguments[1]) == L"--tsf-host") {
        return run_tsf_host();
    }
    if (argument_count == 2 && std::wstring_view(arguments[1]) == L"--tsf-integration") {
        return run_tsf_integration();
    }
    if (argument_count == 2 && std::wstring_view(arguments[1]) == L"--qt-tsf-integration") {
        return run_qt_tsf_integration();
    }
    if (argument_count == 2 && std::wstring_view(arguments[1]) == L"--notepad-tsf-integration") {
        return run_notepad_tsf_integration();
    }
    if (argument_count == 2 && std::wstring_view(arguments[1]) == L"--foreground-integration") {
        return run_foreground_integration();
    }
    if (argument_count == 2) {
        constexpr auto integration_text = L"SENSEVOICE_CURSOR_INJECTION_OK";
        const std::uintptr_t target_window =
            static_cast<std::uintptr_t>(_wcstoui64(arguments[1], nullptr, 10));
        if (target_window == 0) return 1;
        return inject_text_into_windows_text_input(
            {.window = target_window, .focus = 0},
            integration_text,
            GetCurrentProcessId())
            ? 0
            : 1;
    }

    // A recording can finish while the foreground window is the desktop or
    // another non-editable surface. The UI must treat that as a clipboard-only
    // path; the injector itself must reject it without dereferencing a target.
    if (inject_text_into_windows_text_input(
            WindowsTextInputTarget{}, L"no-target", GetCurrentProcessId())) {
        std::cerr << "invalid target unexpectedly accepted text\n";
        return 1;
    }

    HWND parent = CreateWindowExW(
        0, L"STATIC", L"injector-test", WS_OVERLAPPED,
        0, 0, 320, 120, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    HWND edit = CreateWindowExW(
        0, L"EDIT", L"开头待替换结尾", WS_CHILD | ES_AUTOHSCROLL,
        0, 0, 300, 40, parent, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (parent == nullptr || edit == nullptr) {
        std::cerr << "failed to prepare native edit injection test\n";
        return 1;
    }

    const WindowsTextInputTarget saved_target{
        .window = reinterpret_cast<std::uintptr_t>(parent),
        .focus = reinterpret_cast<std::uintptr_t>(edit),
    };
    SendMessageW(edit, EM_SETSEL, 2, 5);
    const bool first_injection = inject_text_into_windows_text_input(
        saved_target,
        tsf_first_text,
        0);
    const bool second_injection = inject_text_into_windows_text_input(
        saved_target,
        tsf_second_text,
        0);
    wchar_t actual[64]{};
    GetWindowTextW(edit, actual, static_cast<int>(std::size(actual)));
    if (!first_injection || !second_injection || std::wstring(actual) != tsf_expected) {
        std::cerr << "saved native edit target did not accept consecutive text\n";
        DestroyWindow(parent);
        return 1;
    }

    const WindowsTextInputTarget mismatched_process_target{
        .window = reinterpret_cast<std::uintptr_t>(parent),
        .focus = reinterpret_cast<std::uintptr_t>(edit),
        .process_id = GetCurrentProcessId() + 1,
    };
    if (inject_text_into_windows_text_input(
            mismatched_process_target, L"stale-process", 0)) {
        std::cerr << "target with a mismatched process identity was accepted\n";
        DestroyWindow(parent);
        return 1;
    }

    DestroyWindow(parent);
    if (inject_text_into_windows_text_input(saved_target, L"destroyed-target", 0)) {
        std::cerr << "destroyed target was unexpectedly accepted\n";
        return 1;
    }
    return 0;
}
