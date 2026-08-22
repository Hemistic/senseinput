# SenseVoice 语音输入（SenseVoice Desk）

![Windows](https://img.shields.io/badge/platform-Windows-0078D4?logo=windows&logoColor=white)
![C++20](https://img.shields.io/badge/C%2B%2B-20-00599C?logo=cplusplus&logoColor=white)
![Qt 6](https://img.shields.io/badge/UI-Qt%206-41CD52?logo=qt&logoColor=white)
![Offline](https://img.shields.io/badge/inference-offline-2E7D32)

轻量、离线、原生 Windows 语音输入工具。

SenseVoice 是把麦克风语音转换为文字并注入当前光标位置的桌面工具，不是传统的语言输入法。按住快捷键说话，松开后即可将本次识别结果写入正在使用的应用。

SenseVoice Desk 使用 SenseVoiceSmall Q8 GGUF 作为底层识别模型，配合 FSMN-VAD 实现实时分句、临时结果更新和最终结果提交。麦克风音频和识别结果都留在本机，不依赖云端 API。

## 项目特点

- **离线优先**：默认模型和所有处理都在本机运行。
- **轻量模型**：SenseVoiceSmall Q8 约 242 MiB，适合桌面常驻。
- **实时体验**：VAD 负责断句，识别线程只处理最新快照，避免结果堆积。
- **语音结果注入**：通过 Windows TSF 和 UI Automation 将识别文字写入当前光标，不依赖剪贴板粘贴。
- **可调参数**：快捷键、VAD 门限、句尾静音、最低响度、SNR、热词和文本整理模式均可配置。
- **低打扰浮窗**：Qt 6 无边框界面显示文字、响度、VAD 状态、声纹和计时。
- **内存保护**：单句长度和工作集都有上限，长内容在最近的 VAD 边界自动分句。
- **音频保护**：录音期间可暂时静音默认播放设备，结束后恢复原状态。

## 快速开始

### 安装程序

从 Releases 下载 `SenseVoice-0.1.1-Setup.exe`，双击安装即可。安装程序会：

- 安装到当前用户的 %LOCALAPPDATA%\\SenseVoice；
- 创建开始菜单快捷方式；
- 默认通过当前用户的 `HKCU\Software\Microsoft\Windows\CurrentVersion\Run` 注册开机启动；
- 不需要管理员权限；
- 支持从“应用和功能”或开始菜单中的 `unins000.exe` 卸载，卸载时会停止程序并清理自启动注册和快捷方式。便携版 ZIP 仍提供 `uninstall.ps1`。

### 便携版 ZIP

解压 Windows ZIP 发布包，运行：

~~~powershell
.\\sensevoice-ui.exe
~~~

模型加载完成后，在任意 Windows 文本输入框中按住默认快捷键 Ctrl+Alt+Space，说话后松开即可注入到录音开始前的光标位置。程序不会自动按 Enter，避免误发送消息。

默认快捷键可以在托盘菜单的“设置”中改为 Ctrl+Win、Ctrl+Shift+Space、F8 或自定义组合键；同一页面也可以关闭“开机自动启动”。

## 工作原理

SenseVoiceSmall 不是原生流式模型，项目通过短句快照实现实时体验：

~~~text
麦克风 → 音频整形 → FSMN-VAD → 短句快照 → SenseVoiceSmall → partial/final
~~~

临时结果会持续更新当前预览，VAD 确认句尾后输出最终结果，已提交音频随即释放，下一句从新的短窗口开始。识别线程忙时只保留最新快照，不排队过时结果。

## 功能

### 文本处理

- 原文模式：保留模型输出；
- 精简模式：删除常见口水词、统一中文标点、合并相邻重复句；
- 热词表：标准词、别名、启用状态、命中计数和增强强度；
- 词典：支持 dict\\user.dict.utf8 补充专有名词；
- 纠正规则：支持确定性字面替换和单个 {num} 数字占位符。

### 音频与内存控制

- 录音采集层只做衰减，不放大背景噪声；
- 目标语音 RMS 约为 -20 dBFS，峰值保护线为 -4 dBFS；
- 默认单句最长 15 秒，达到内存保护线时在最近 VAD 边界分句；
- 默认运行时内存保护线为 300 MiB；
- 录音结束、取消、失败和程序退出都会恢复播放设备原来的静音状态。

## 命令行

### 麦克风输入

~~~powershell
.\\sensevoice-stream.exe --model .\\models\\sensevoice-small-q8.gguf --vad .\\models\\fsmn-vad.gguf --mic
~~~

### 音频文件

~~~powershell
.\\sensevoice-stream.exe --model .\\models\\sensevoice-small-q8.gguf --vad .\\models\\fsmn-vad.gguf --audio 'C:\\path\\to\\recording.m4a'
~~~

常用参数：

| 参数 | 默认值 | 作用 |
| --- | ---: | --- |
| --partial-ms | 450 | 临时结果更新的目标间隔 |
| --endpoint-silence-ms | 700 | 确认句子结束所需的静音时长 |
| --maximum-utterance-ms | 15000 | 单句允许处理的最大音频时长 |
| --memory-limit-mb | 300 | 工作集内存保护上限 |
| --vad-speech-threshold | profile | FSMN 语音置信度门限 |
| --vad-min-db | profile | 输入响度绝对下限 |
| --vad-min-snr-db | profile | 最低信噪比余量 |
| --hotwords FILE | 可选 | UTF-8 制表符分隔的热词文件 |
| --corrections FILE | 可选 | 确定性的文本纠正规则 |

## 构建

环境要求：

- Windows 10 或更高版本；
- 安装带有“使用 C++ 的桌面开发”和 CMake 的 Visual Studio 2022；
- 使用相同 MSVC 架构构建的 Qt 6.8 Widgets；
- 已初始化 Git 子模块。

~~~powershell
git submodule update --init --recursive
cmake -S . -B build -G 'Visual Studio 17 2022' -A x64 -DCMAKE_PREFIX_PATH='D:\\Qt\\6.8.3\\msvc2022_64'
cmake --build build --config Release --target sensevoice-ui sensevoice-ui-legacy sensevoice-stream --parallel 8
ctest --test-dir build -C Release --output-on-failure
~~~

默认构建使用 CPU 后端。为避免额外的运行时依赖，项目关闭了 OpenMP；Release 构建针对支持 AVX2/FMA/F16C 的 CPU 做了优化。

## 打包

~~~powershell
.\tools\package_windows.ps1 -Version 0.1.1
.\tools\create_installer.ps1 -Version 0.1.1
~~~

输出文件：

~~~text
out\\SenseVoice-0.1.1-windows-x64.zip
out\\SenseVoice-0.1.1-Setup.exe
~~~

安装程序是标准的 Inno Setup 向导，包含欢迎、安装位置、开始菜单、开机启动、进度和完成页面。它以当前用户权限安装，不需要管理员权限，会在“应用和功能”中注册卸载项，并创建原生的 `unins000.exe` 卸载程序。便携版 ZIP 会单独保留 PowerShell 安装和卸载脚本。

要构建标准安装程序，请安装 Inno Setup 6/7 并确保可以找到 `ISCC.exe`，设置 `INNO_SETUP_HOME`，或向 `create_installer.ps1` 传入 `-CompilerPath`。

## 性能参考

以下数据是在一台拥有 20 个逻辑核心、支持 AVX2 的 Windows 设备上使用 Q8 模型测得的：

- 模型文件：约 244 MiB；
- 模型加载：约 80-140 ms；
- 首个非空临时结果：开始说话后约 700 ms；
- VAD 最终确认：约 30-40 ms；
- SenseVoice 最终推理：约 370-400 ms；
- 实时回放：总 CPU 占用约 13%；
- 峰值工作集：当前轻量文本处理配置下约 289 MiB。

以上数据与硬件有关，仅作为工程参考，不构成统一基准测试结论。

## 项目结构

~~~text
src/                         C++20 识别、VAD、音频和 Qt 界面
resources/                   Qt 资源、ICO 图标和 Windows 版本资源
packaging/windows/           Inno Setup 定义和便携版脚本
tools/                       打包、图标生成和几何检查工具
third_party/                 llama.cpp 运行时核心、FunASR、cppjieba 和 OpenLess 源码
tests/                       识别、文本处理和 Windows 注入测试
~~~

## 模型与第三方致谢

默认模型是 FunAudioLLM 发布的 SenseVoiceSmall Q8 GGUF。运行时基于 llama.cpp 和 FunASR 组件构建，文本分词使用兼容 cppjieba 的词典。重新分发修改后的二进制文件或模型前，请查阅各上游仓库和模型的分发条款。

## 开发路线

- [x] 离线 SenseVoiceSmall Q8 推理
- [x] FSMN-VAD 断句和可调门限
- [x] 热词管理和本地文本整理
- [x] Windows TSF/UI Automation 注入
- [x] Qt 浮窗界面和标准 Inno Setup 安装程序
- [ ] macOS 音频采集和文本注入
- [ ] 针对支持硬件的可选 FP16 性能配置

## 参与贡献

欢迎提交 Issue 和 Pull Request。报告识别或注入问题时，请附上 Windows 版本、CPU 架构、模型量化类型、音频特征和相关日志。请勿上传私人录音或凭据。

## 可选的 OpenRouter TTS

仓库包含 tools/openrouter_tts.py，可调用 OpenRouter 的 OpenAI 兼容语音接口生成播客或语音片段。API key 只从环境变量 OPENROUTER_API_KEY 读取：

~~~powershell
$env:OPENROUTER_API_KEY = 'sk-or-v1-...'
py -3 tools/openrouter_tts.py `
  --text-file examples/podcast_dialogue.zh.txt `
  --output out/sensevoice-desk-podcast-dialogue.mp3
~~~

默认模型是 fish-audio/s2.1-pro-free:free，输出 MP3。默认不指定 voice，让 Fish Audio 选择模型默认音色；如果需要显式指定提供方支持的声音，可以额外传入 --voice。直连失败时可传入 --proxy http://127.0.0.1:17890，或设置 OPENROUTER_PROXY。不要把 API key 或本地 .env 文件提交到仓库。

要生成更像播客主持人的表达，可以增加 --instructions，说明开场热情、重点强调、提问上扬、回答沉稳和适中的语速。

## 许可证

本仓库目前没有声明顶层许可证。发布衍生二进制文件前，请分别确认本项目、捆绑的第三方源码和 SenseVoice 模型的许可证要求。
