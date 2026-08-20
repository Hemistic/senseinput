#pragma once

#include <cstdint>
#include <memory>
#include <string_view>

struct WindowsTextInputTargetState;

struct WindowsTextInputTarget {
    std::uintptr_t window = 0;
    std::uintptr_t focus = 0;
    std::shared_ptr<WindowsTextInputTargetState> state;

    [[nodiscard]] bool valid() const {
        return window != 0;
    }
};

WindowsTextInputTarget capture_windows_text_input_target(std::uint32_t excluded_process_id);
bool windows_text_input_target_has_tsf_session(const WindowsTextInputTarget& target);
bool inject_text_into_windows_text_input(
    WindowsTextInputTarget target,
    std::wstring_view text,
    std::uint32_t excluded_process_id);
