# SenseVoice 本地实时语音识别核心

这是一个离线、原生 C++ 的 Windows 实时识别内核。底模固定为
SenseVoiceSmall Q8 GGUF，FSMN-VAD 负责过滤静音和自动断句。两份模型在进程启动时
加载一次，录音期间常驻内存。

SenseVoiceSmall 本身是离线 CTC 模型。本项目的“实时”实现方式是持续识别当前短句，
输出可覆盖的 `partial`；VAD 确认句尾后输出 `final`，丢弃已提交音频并开始下一句。
因此它不会伪装成原生流式模型，也不会在长时间开启麦克风时无限积累静音。

## 已部署版本

可直接运行的目录是 `D:\Project\sensevoice-input\dist`：

- `sensevoice-ui.exe`：Qt 6 实现的轻量悬浮听写界面。
- `sensevoice-ui-legacy.exe`：原来的原生 Windows 调试界面。
- `sensevoice-stream.exe`：静态编译的 AVX2/FMA/F16C CPU 程序。
- `models\sensevoice-small-q8.gguf`：SenseVoiceSmall Q8，242.43 MiB。
- `models\fsmn-vad.gguf`：FSMN-VAD，1.64 MiB。
- `start-microphone.cmd`：双击后使用默认麦克风，按 Enter 结束最后一句。
- `transcribe-file.cmd`：把音频文件拖到脚本上进行实时回放识别。

首次体验直接双击 `sensevoice-ui.exe`。程序空闲时只保留 Windows 托盘图标；模型在后台加载完成后，在其他软件的输入框中按住
`Ctrl+Alt+Space` 约 280 ms 开始说话，松开后完成识别；最终文字会复制到剪贴板，并粘贴回开始
录音前的光标位置。程序不会自动按回车发送，避免误发消息。点击控制条右侧按钮也能开始或
完成，松开快捷键会先注入到原光标位置，再自动隐藏输入窗。需要手动操作时可从托盘菜单
显示输入窗；右键菜单可以取消本次输入。右键或托盘打开设置可以改成 `Ctrl+Win`、`Ctrl+Shift+Space`、
`F8` 或自定义单键组合。

上方气泡实时显示当前 partial 和已确认 final，宽度和高度会随文字量增长；达到最大宽度
后自动换行并向下扩展。气泡使用与实际绘制一致的中文排版结果计算尺寸，始终显示完整文本，
不截断也不滚动。窗口外层完全透明，只保留浅色转写面板和深色控制条。控制条提供取消、
实时声纹、录音计时、输出模式和完成操作；声纹以 50 ms 间隔显示实时响度，悬停时可查看输入 dBFS、VAD 状态和当前启动门限。悬浮窗使用
不激活窗口样式，不会抢走输入焦点。

右键悬浮窗可打开 `设置...`：在“输入”页调整按住说话的组合键并选择 `原文` 或 `精简`，
在“热词”页维护标准词、别名、启用状态和 CTC 增强强度，在“VAD”页调整句尾静音、
FSMN 模型阈值、最低响度和底噪余量。设置会持久保存并在下一次录音时使用。托盘和右键菜单
保留最近 20 条最终输入，点击即可重新复制；完整调试指标仍可通过 `sensevoice-ui-legacy.exe` 查看。

便携目录已经带有 Qt 6 Widgets 和 MSVC 运行库，不要求另行安装 Qt、Visual C++ 或
OpenMP。WAV 等常见格式由 miniaudio 解码；M4A/AAC 使用 Windows Media Foundation
回退，因此包含中文、空格和括号的路径可以直接使用。

## 命令行

麦克风：

```powershell
.\build\Release\sensevoice-stream.exe `
  --model D:\Project\zed\.local\funasr-sensevoice\gguf\sensevoice-small-q8.gguf `
  --vad D:\Project\zed\.local\funasr-sensevoice\gguf\fsmn-vad.gguf `
  --mic
```

原始 M4A 文件：

```powershell
.\build\Release\sensevoice-stream.exe `
  --model D:\Project\zed\.local\funasr-sensevoice\gguf\sensevoice-small-q8.gguf `
  --vad D:\Project\zed\.local\funasr-sensevoice\gguf\fsmn-vad.gguf `
  --audio 'C:\Users\41448\Documents\录音\录音 (2).m4a'
```

常用参数：

- `--partial-ms 450`：两次临时结果之间的目标间隔。
- `--endpoint-silence-ms 700`：自动结束一句所需的静音长度。
- 麦克风默认抗噪档：新句需连续 300 ms 语音，FSMN 置信边界 0.8、最低能量
  -45 dBFS、相对噪声底至少 8 dB；文件转写保持兼容档，避免削弱已有录音。
- `--maximum-utterance-ms 10000`：单句最大音频预算；超出时优先在最近 VAD 边界收句。
- `--memory-limit-mb 300`：WorkingSet 保护线；按开始听写时的常驻基线加 32 MiB
  运行余量计算，达到后在最近的 VAD 边界分句。这样不会因为模型本身的常驻内存
  已经超过保护线而从第一秒开始误切句。
- `--hotwords FILE`：加载制表符分隔的热词文件，支持别名、启用状态和命中计数。
- `--corrections FILE`：加载确定性纠正规则；支持字面替换和单个 `{num}` 数字占位符。

如果麦克风仍把环境声当成语音，可以继续提高 `--vad-speech-threshold` 或
`--vad-min-snr-db`；如果漏掉轻声，再降低这两个参数。`--vad-min-db` 控制新句的
绝对能量下限。

热词文件使用 UTF-8 制表符格式：`词条<TAB>别名1|别名2<TAB>启用(1/0)<TAB>命中数<TAB>增强强度`。
程序会在会话结束时回写命中计数。便携版自带 `dict` 目录中的常用词典；程序使用轻量
最大匹配分词，`dict\user.dict.utf8` 可以直接补充新词，词典缺失时会自动降级为规则处理。
图形界面会自动加载程序同目录的 `hotwords.tsv` 和 `corrections.tsv`（如果存在）；热词别名
会同时修正临时预览和最终结果。热词只应用于确实固定的专有名词，不用于掩盖普通同音词
的声学识别问题。

`精简`模式会在最终注入前运行保守的本地整理：删除带停顿标记的“嗯/呃/额”等填充词、统一中文标点、
移除相邻完全重复句，并在长内容的句界处分段；`原文`模式保留识别输出。这些处理不改写词义。“自动归纳分点”和
“调整不通顺语序”需要独立语言模型，目前没有用脆弱的替换规则冒充这两项能力。

麦克风模式会在采集层抑制过响输入：仅衰减、不放大背景，目标语音 RMS 为 -20 dBFS，
峰值上限为 -4 dBFS。界面显示原始输入响度、峰值及自动衰减量。如果出现“输入削波”，
说明失真发生在系统采集之前，需要降低 Windows 麦克风音量；后置衰减无法恢复已削掉的细节。
- `--threads 8`：SenseVoice 和 VAD 的 CPU 线程数。
- `--replay-speed 4`：文件测试时以四倍速送入音频。
- `--endpoint-silence-ms 2000`：复现官方 FSMN-VAD 默认端点行为。

## 输出协议

标准输出是逐行 JSON。`partial` 会变化，输入法前端应覆盖当前预览；`final` 已由 VAD
或手动停止锁定，可以提交到编辑器。`stable` 是多次识别共有且扣除尾部保护字符后的
稳定前缀，`unstable` 是仍可能变化的尾部。

```json
{"event":"partial","sequence":8,"audio_ms":4380,"vad_ms":9,"inference_ms":59,"text":"微信输入法。","stable":"","unstable":"微信输入法。","stability_conflict":false}
```

`vad_ms` 和 `inference_ms` 是本次 VAD 与 SenseVoice 的耗时，不包含文件回放等待时间。
识别线程忙时不排队旧 partial，只处理最新快照。

## 当前实测

测试音频时长 17.045 秒，CPU 为 20 逻辑线程的 AVX2 机器：

- 模型文件总计 244.07 MiB；程序约 2 MiB。
- Q8 模型加载约 80-140 ms；VAD 模型加载低于 1 ms。
- 首段语音从约 3.12 秒开始，首个非空 partial 在约 3.82 秒，开口后约 700 ms。
- 最终 VAD 约 30-40 ms，SenseVoice 约 370-400 ms。
- 实时回放平均约占 2.5 个逻辑核，即该 20 线程机器总 CPU 的约 13%。
- 当前轻量分词版实测峰值工作集约 289 MiB；10 秒长句推理的峰值私有提交约 298 MiB，
  推理完成后回落到约 259 MiB。
- 麦克风环形缓冲实测没有丢帧。

模型文件小于 300 MB，但运行时内存不是 300 MB。SenseVoice 的长句编码器工作区会随
句长增大；默认 700 ms 自动端点、10 秒单句预算和基于启动基线的 WorkingSet 保护线
用于限制长期增长。模型和词典的固定常驻内存不计入增长触发，但会保留 32 MiB 推理余量。

## 构建与测试

Visual Studio 2022 和 Qt 6.8：

```powershell
cmake -S . -B build -G 'Visual Studio 17 2022' -A x64 `
  -DCMAKE_PREFIX_PATH=D:\Qt\6.8.3\msvc2022_64
cmake --build build --config Release `
  --target sensevoice-ui sensevoice-ui-legacy sensevoice-stream `
  text-processor-test stability-tracker-test audio-conditioner-test --parallel 8
ctest --test-dir build -C Release --output-on-failure
```

构建固定关闭 OpenMP 动态依赖，并使用与 Qt 一致的动态 MSVC 运行库。Qt 前端只依赖
Core、GUI、Widgets 和 `qwindows` 平台插件，不包含 QML、WebEngine 或网络模块。当前仅
启用 CPU 后端；CUDA 版本需要先解决对应 CUDA DLL 的部署，现阶段不作为正式路径。

## Windows 打包与安装

使用下面的命令生成便携 ZIP。构建目录中的 Release 程序会覆盖 `dist` 中对应的三个
可执行文件，模型和 Qt 运行库继续从 `dist` 复制到发布目录：

```powershell
.\tools\package_windows.ps1 -Version 0.1.0
```

产物位于 `out\SenseVoice-0.1.0-windows-x64.zip`。解压后运行 `install.cmd`，安装程序
会把应用复制到当前用户的 `%LOCALAPPDATA%\SenseVoice`，创建开始菜单快捷方式，并默认
注册到当前用户的 Startup 文件夹，因此不需要管理员权限。使用 `install.cmd -NoStartup`
可以关闭开机启动。应用和托盘使用同一套 SenseVoice 图标，图标资源为多尺寸 ICO，适配
高 DPI 任务栏；卸载可以从“应用和功能”进入，也可以直接运行安装目录里的
`uninstall.ps1`。

Git 初始提交不包含 `build`、`dist`、`out`、截图和模型发布目录；第三方源代码保留在
`third_party` 以保证本地构建可复现，临时的 llama.cpp 压缩包和不完整快照会被忽略。
