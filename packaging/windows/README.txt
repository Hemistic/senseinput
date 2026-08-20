SenseVoice Windows 语音输入工具

SenseVoice 是本地语音输入工具，不是传统的语言输入法。按住快捷键说话，
松开后会把识别出的文字注入当前光标位置。

标准安装程序是 `SenseVoice-*-Setup.exe`。它会打开正常的安装向导，包含安装
位置、开始菜单、开机启动、安装进度和完成页面。程序按当前 Windows 用户安装，
不需要管理员权限。生成的 `unins000.exe` 和“应用和功能”中的卸载项提供标准
卸载流程。卸载时会停止 SenseVoice、删除 `HKCU Run` 自启动项和旧的启动文件夹
快捷方式，然后删除已安装文件。

在标准安装向导中，取消 `开机自动启动` 选项即可关闭开机启动。

仅便携版 ZIP 继续提供旧的脚本安装流程：
  `install.cmd`

不注册开机启动安装便携版 ZIP：
  `install.cmd -NoStartup`
  `powershell.exe -ExecutionPolicy Bypass -File uninstall.ps1`
