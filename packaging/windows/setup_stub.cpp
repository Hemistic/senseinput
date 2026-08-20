#include <windows.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

constexpr std::size_t kMagicSize = 20;
constexpr std::size_t kTrailerSize = kMagicSize + sizeof(std::int64_t) + sizeof(std::int64_t);
constexpr char kMagicText[] = "SENSEVOICE_SETUP_V1";

std::wstring quoteWindowsArgument(const std::wstring& value) {
    std::wstring result = L"\"";
    std::size_t backslashes = 0;
    for (const wchar_t character : value) {
        if (character == L'\\') {
            ++backslashes;
            continue;
        }
        if (character == L'\"') {
            result.append(backslashes * 2 + 1, L'\\');
            result.push_back(L'\"');
            backslashes = 0;
            continue;
        }
        result.append(backslashes, L'\\');
        backslashes = 0;
        result.push_back(character);
    }
    result.append(backslashes * 2, L'\\');
    result.push_back(L'\"');
    return result;
}

std::wstring quotePowerShellLiteral(const std::wstring& value) {
    std::wstring result = L"'";
    for (const wchar_t character : value) {
        result.push_back(character);
        if (character == L'\'') {
            result.push_back(L'\'');
        }
    }
    result.push_back(L'\'');
    return result;
}

bool readTrailer(const std::filesystem::path& executable,
                 std::int64_t& payloadOffset,
                 std::int64_t& payloadSize) {
    std::error_code error;
    const auto fileSize = std::filesystem::file_size(executable, error);
    if (error || fileSize < kTrailerSize) {
        return false;
    }

    std::ifstream input(executable, std::ios::binary);
    if (!input) {
        return false;
    }
    input.seekg(static_cast<std::streamoff>(fileSize - kTrailerSize));

    std::array<char, kMagicSize> magic{};
    input.read(magic.data(), static_cast<std::streamsize>(magic.size()));
    if (!input || std::string(magic.data(), 19) != kMagicText || magic[19] != '\0') {
        return false;
    }

    input.read(reinterpret_cast<char*>(&payloadOffset), sizeof(payloadOffset));
    input.read(reinterpret_cast<char*>(&payloadSize), sizeof(payloadSize));
    if (!input || payloadOffset < 0 || payloadSize <= 0) {
        return false;
    }
    if (static_cast<std::uint64_t>(payloadOffset) + static_cast<std::uint64_t>(payloadSize) > fileSize - kTrailerSize) {
        return false;
    }
    return true;
}

bool copyPayload(const std::filesystem::path& executable,
                 const std::filesystem::path& payload,
                 std::int64_t payloadOffset,
                 std::int64_t payloadSize) {
    std::ifstream input(executable, std::ios::binary);
    std::ofstream output(payload, std::ios::binary | std::ios::trunc);
    if (!input || !output) {
        return false;
    }
    input.seekg(static_cast<std::streamoff>(payloadOffset));

    std::vector<char> buffer(1024 * 1024);
    std::int64_t remaining = payloadSize;
    while (remaining > 0) {
        const auto requested = static_cast<std::streamsize>(std::min<std::int64_t>(remaining, buffer.size()));
        input.read(buffer.data(), requested);
        const auto readCount = input.gcount();
        if (readCount <= 0) {
            return false;
        }
        output.write(buffer.data(), readCount);
        if (!output) {
            return false;
        }
        remaining -= readCount;
    }
    return true;
}

bool runPowerShell(const std::filesystem::path& archive,
                   const std::filesystem::path& extractionDirectory,
                   DWORD& exitCode) {
    const auto archiveLiteral = quotePowerShellLiteral(archive.wstring());
    const auto directoryLiteral = quotePowerShellLiteral(extractionDirectory.wstring());
    const auto installScript = quotePowerShellLiteral((extractionDirectory / L"install.ps1").wstring());
    const std::wstring script =
        L"$ErrorActionPreference='Stop'; "
        L"Expand-Archive -LiteralPath " + archiveLiteral +
        L" -DestinationPath " + directoryLiteral + L" -Force; "
        L"& " + installScript + L"; "
        L"exit $LASTEXITCODE";

    std::wstring commandLine = L"powershell.exe -NoLogo -NoProfile -NonInteractive "
                               L"-ExecutionPolicy Bypass -Command " + quoteWindowsArgument(script);
    std::vector<wchar_t> commandBuffer(commandLine.begin(), commandLine.end());
    commandBuffer.push_back(L'\0');

    STARTUPINFOW startupInfo{};
    startupInfo.cb = sizeof(startupInfo);
    PROCESS_INFORMATION processInfo{};
    const BOOL created = CreateProcessW(nullptr, commandBuffer.data(), nullptr, nullptr, FALSE,
                                        CREATE_NO_WINDOW, nullptr, nullptr, &startupInfo, &processInfo);
    if (!created) {
        return false;
    }

    WaitForSingleObject(processInfo.hProcess, INFINITE);
    const BOOL gotExitCode = GetExitCodeProcess(processInfo.hProcess, &exitCode);
    CloseHandle(processInfo.hThread);
    CloseHandle(processInfo.hProcess);
    return gotExitCode == TRUE;
}

std::filesystem::path makeTemporaryDirectory() {
    std::error_code error;
    const auto base = std::filesystem::temp_directory_path(error);
    if (error) {
        return {};
    }
    const auto processId = static_cast<unsigned long>(GetCurrentProcessId());
    for (unsigned int attempt = 0; attempt < 100; ++attempt) {
        const auto candidate = base / (L"SenseVoiceSetup-" + std::to_wstring(processId) + L"-" +
                                       std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(attempt));
        if (std::filesystem::create_directories(candidate, error) && !error) {
            return candidate;
        }
        error.clear();
    }
    return {};
}

int showFailure(const std::wstring& message) {
    MessageBoxW(nullptr, message.c_str(), L"SenseVoice 安装程序", MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
    return 1;
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR commandLine, int) {
    wchar_t executableBuffer[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(nullptr, executableBuffer, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        return showFailure(L"无法定位安装包文件。");
    }

    const std::filesystem::path executable(std::wstring(executableBuffer, length));
    std::int64_t payloadOffset = 0;
    std::int64_t payloadSize = 0;
    if (!readTrailer(executable, payloadOffset, payloadSize)) {
        return showFailure(L"安装包内容不完整或已损坏，请重新下载。");
    }

    const auto temporaryDirectory = makeTemporaryDirectory();
    if (temporaryDirectory.empty()) {
        return showFailure(L"无法创建临时目录。");
    }

    const auto archive = temporaryDirectory / L"payload.zip";
    if (!copyPayload(executable, archive, payloadOffset, payloadSize)) {
        std::error_code ignored;
        std::filesystem::remove_all(temporaryDirectory, ignored);
        return showFailure(L"无法读取安装包内容。");
    }

    DWORD exitCode = ERROR_GEN_FAILURE;
    const bool processStarted = runPowerShell(archive, temporaryDirectory, exitCode);
    std::error_code ignored;
    std::filesystem::remove_all(temporaryDirectory, ignored);
    if (!processStarted) {
        return showFailure(L"无法启动安装组件，请确认 Windows PowerShell 可用。");
    }
    if (exitCode != ERROR_SUCCESS) {
        std::wstringstream message;
        message << L"安装失败，错误代码：" << exitCode;
        return showFailure(message.str());
    }

    const std::wstring arguments = commandLine != nullptr ? commandLine : L"";
    if (arguments.find(L"/quiet") == std::wstring::npos &&
        arguments.find(L"/silent") == std::wstring::npos) {
        MessageBoxW(nullptr, L"SenseVoice 已安装完成。", L"SenseVoice 安装程序", MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
    }
    return 0;
}
