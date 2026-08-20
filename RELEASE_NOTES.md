# SenseVoice Desk v0.1.0

首个公开 Windows 版本。

## Included

- SenseVoiceSmall Q8 GGUF 离线识别
- FSMN-VAD 实时分句和可调阈值
- Qt 6 浮窗，显示实时文字、响度和 VAD 状态
- Windows TSF/UI Automation 光标注入
- 热词增强、口水词清理、标点和重复句整理
- 单句长度与工作集内存保护
- 录音期间暂时静音默认播放设备，结束后恢复
- 用户级安装程序和免安装 Portable ZIP

## Assets

- `SenseVoice-0.1.0-Setup.exe`: 用户级安装包，默认注册当前用户开机启动
- `SenseVoice-0.1.0-windows-x64.zip`: 免安装便携版

## Notes

- 当前 Release 仅支持 Windows x64。
- 默认使用 SenseVoiceSmall Q8，模型和运行时均在本机执行。
- 重新分发模型、运行时和第三方组件时，请遵守各上游项目及模型的许可条款。
