#include "windows_text_injector.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>

#include <iostream>
#include <string>

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

} // namespace

int wmain() {
    const std::wstring text = commandLineText();
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
