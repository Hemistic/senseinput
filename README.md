# SenseVoice Desk

![Windows](https://img.shields.io/badge/platform-Windows-0078D4?logo=windows&logoColor=white)
![C++20](https://img.shields.io/badge/C%2B%2B-20-00599C?logo=cplusplus&logoColor=white)
![Qt 6](https://img.shields.io/badge/UI-Qt%206-41CD52?logo=qt&logoColor=white)
![Offline](https://img.shields.io/badge/inference-offline-2E7D32)

轻量、离线、原生 Windows 语音输入工具。

SenseVoice Desk 使用 SenseVoiceSmall Q8 GGUF 作为底层识别模型，配合 FSMN-VAD 实现实时分句、临时结果更新和最终结果提交。麦克风音频和识别结果都留在本机，不依赖云端 API。

## Highlights

- **离线优先**：默认模型和所有处理都在本机运行。
- **轻量模型**：SenseVoiceSmall Q8 约 242 MiB，适合桌面常驻。
- **实时体验**：VAD 负责断句，识别线程只处理最新快照，避免结果堆积。
- **直接输入**：通过 Windows TSF 和 UI Automation 写入当前光标，不依赖剪贴板粘贴。
- **可调参数**：快捷键、VAD 门限、句尾静音、最低响度、SNR、热词和文本整理模式均可配置。
- **低打扰浮窗**：Qt 6 无边框界面显示文字、响度、VAD 状态、声纹和计时。
- **内存保护**：单句长度和工作集都有上限，长内容在最近的 VAD 边界自动分句。
- **音频保护**：录音期间可暂时静音默认播放设备，结束后恢复原状态。

## Quick Start

### Installer

从 Releases 下载 SenseVoice-Setup.exe，双击安装即可。安装程序会：

- 安装到当前用户的 %LOCALAPPDATA%\\SenseVoice；
- 创建开始菜单快捷方式；
- 默认通过当前用户的 `HKCU\Software\Microsoft\Windows\CurrentVersion\Run` 注册开机启动；
- 不需要管理员权限；
- 支持从“应用和功能”或安装目录中的 `uninstall.ps1` 卸载，卸载时会停止程序并清理自启动注册和快捷方式。

### Portable ZIP

解压 Windows ZIP 发布包，运行：

~~~powershell
.\\sensevoice-ui.exe
~~~

模型加载完成后，在任意 Windows 文本输入框中按住默认快捷键 Ctrl+Alt+Space，说话后松开即可注入到录音开始前的光标位置。程序不会自动按 Enter，避免误发送消息。

默认快捷键可以在托盘菜单的“设置”中改为 Ctrl+Win、Ctrl+Shift+Space、F8 或自定义组合键；同一页面也可以关闭“开机自动启动”。

## How It Works

SenseVoiceSmall 不是原生流式模型，项目通过短句快照实现实时体验：

~~~text
麦克风 → 音频整形 → FSMN-VAD → 短句快照 → SenseVoiceSmall → partial/final
~~~

partial 会持续更新当前预览，VAD 确认句尾后输出 final，已提交音频随即释放，下一句从新的短窗口开始。识别线程忙时只保留最新快照，不排队过时结果。

## Features

### Text processing

- 原文模式：保留模型输出；
- 精简模式：删除常见口水词、统一中文标点、合并相邻重复句；
- 热词表：标准词、别名、启用状态、命中计数和增强强度；
- 词典：支持 dict\\user.dict.utf8 补充专有名词；
- 纠正规则：支持确定性字面替换和单个 {num} 数字占位符。

### Audio and memory controls

- 录音采集层只做衰减，不放大背景噪声；
- 目标语音 RMS 约为 -20 dBFS，峰值保护线为 -4 dBFS；
- 默认单句最长 15 秒，达到内存保护线时在最近 VAD 边界分句；
- 默认运行时内存保护线为 300 MiB；
- 录音结束、取消、失败和程序退出都会恢复播放设备原来的静音状态。

## Command Line

### Microphone

~~~powershell
.\\sensevoice-stream.exe --model .\\models\\sensevoice-small-q8.gguf --vad .\\models\\fsmn-vad.gguf --mic
~~~

### Audio file

~~~powershell
.\\sensevoice-stream.exe --model .\\models\\sensevoice-small-q8.gguf --vad .\\models\\fsmn-vad.gguf --audio 'C:\\path\\to\\recording.m4a'
~~~

Useful parameters:

| Parameter | Default | Purpose |
| --- | ---: | --- |
| --partial-ms | 450 | Target interval between partial results |
| --endpoint-silence-ms | 700 | Silence required to finalize a sentence |
| --maximum-utterance-ms | 15000 | Maximum audio budget for one utterance |
| --memory-limit-mb | 300 | Working-set protection line |
| --vad-speech-threshold | profile | FSMN speech confidence threshold |
| --vad-min-db | profile | Absolute minimum input level |
| --vad-min-snr-db | profile | Minimum signal-to-noise margin |
| --hotwords FILE | optional | UTF-8 tab-separated hotword file |
| --corrections FILE | optional | Deterministic correction rules |

## Build

Requirements:

- Windows 10 or newer;
- Visual Studio 2022 with Desktop C++ and CMake;
- Qt 6.8 Widgets built for the same MSVC architecture;
- Git submodules initialized.

~~~powershell
git submodule update --init --recursive
cmake -S . -B build -G 'Visual Studio 17 2022' -A x64 -DCMAKE_PREFIX_PATH='D:\\Qt\\6.8.3\\msvc2022_64'
cmake --build build --config Release --target sensevoice-ui sensevoice-ui-legacy sensevoice-stream --parallel 8
ctest --test-dir build -C Release --output-on-failure
~~~

The default build uses the CPU backend. OpenMP is disabled to avoid an extra runtime dependency; the release path is optimized for AVX2/FMA/F16C CPUs.

## Packaging

~~~powershell
.\\tools\\package_windows.ps1 -Version 0.1.0
.\\tools\\create_installer.ps1 -Version 0.1.0
~~~

Outputs:

~~~text
out\\SenseVoice-0.1.0-windows-x64.zip
out\\SenseVoice-0.1.0-Setup.exe
~~~

The installer is a project-owned self-extracting executable. It does not depend on IExpress and does not require administrator rights.

## Performance Snapshot

Measured on a 20-logical-core AVX2 Windows machine with the Q8 model:

- model files: about 244 MiB;
- model load: about 80-140 ms;
- first non-empty partial: about 700 ms after speech starts;
- final VAD: about 30-40 ms;
- final SenseVoice inference: about 370-400 ms;
- real-time replay: about 13% total CPU;
- peak working set: about 289 MiB in the current lightweight text-processing profile.

These numbers are hardware-dependent engineering references, not benchmark claims.

## Project Layout

~~~text
src/                         C++20 recognition, VAD, audio and Qt UI
resources/                   Qt resources, ICO and Windows version resources
packaging/windows/            User-level installer and uninstall scripts
tools/                       Packaging, icon generation and geometry checks
third_party/                 llama.cpp, FunASR runtime, cppjieba and OpenLess sources
tests/                       Recognition, text processing and Windows injection tests
~~~

## Model and Third-Party Credits

The default model is SenseVoiceSmall Q8 GGUF from FunAudioLLM. The runtime is built on llama.cpp and FunASR components; text segmentation uses cppjieba-compatible dictionaries. Check upstream repositories and model distribution terms before redistributing modified binaries or models.

## Roadmap

- [x] Offline SenseVoiceSmall Q8 inference
- [x] FSMN-VAD endpointing and configurable thresholds
- [x] Hotword management and local text cleanup
- [x] Windows TSF/UI Automation injection
- [x] Qt floating UI and self-extracting installer
- [ ] macOS audio capture and text insertion
- [ ] Optional FP16 performance profile for supported hardware

## Contributing

Issues and pull requests are welcome. Please include the Windows version, CPU architecture, model quantization, audio characteristics, and relevant logs when reporting recognition or injection problems. Do not attach private recordings or credentials.

## License

This repository currently does not declare a top-level license. Review the licenses of this project, the bundled third-party sources, and the SenseVoice model before publishing derivative binaries.
