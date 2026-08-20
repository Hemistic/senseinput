#include "windows_text_injector.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>

#include <iostream>
#include <iterator>
#include <string>
#include <string_view>

namespace {

std::wstring commandLineText() {
    int argument_count = 0;
    LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &argument_count);
    if (arguments == nullptr) return {};

    std::wstring text;
    for (int index = 1; index < argument_count; ++index) {
        if (index > 1) text.push_back(L' ');
        text += arguments[index];
    }
    LocalFree(arguments);
    return text;
}

std::wstring utf8StdinText() {
    std::string input(
        (std::istreambuf_iterator<char>(std::cin)),
        std::istreambuf_iterator<char>());
    if (input.empty() || input.size() > static_cast<std::size_t>(INT_MAX)) return {};
    const int required = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, input.data(), static_cast<int>(input.size()),
        nullptr, 0);
    if (required <= 0) return {};
    std::wstring output(static_cast<std::size_t>(required), L'\0');
    if (MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, input.data(), static_cast<int>(input.size()),
            output.data(), required) != required) {
        return {};
    }
    return output;
}

} // namespace

int wmain(int argument_count, wchar_t* arguments[]) {
    const bool read_utf8_stdin = argument_count == 2 &&
        std::wstring_view(arguments[1]) == L"--stdin-utf8";
    const std::wstring text = read_utf8_stdin ? utf8StdinText() : commandLineText();
    if (text.empty()) {
        std::wcerr << L"Usage: sensevoice-inject.exe <text>\n";
        return 2;
    }

    const WindowsTextInputTarget target = capture_windows_text_input_target(GetCurrentProcessId());
    if (!target.valid()) {
        std::wcerr << L"No focused text input target was found.\n";
        return 3;
    }
    if (!inject_text_into_windows_text_input(target, text, GetCurrentProcessId())) {
        std::wcerr << L"Text injection failed.\n";
        return 4;
    }
    return 0;
}
