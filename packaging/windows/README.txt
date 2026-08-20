SenseVoice Windows package

The standard installer is `SenseVoice-*-Setup.exe`. It opens a normal install
wizard with destination, Start Menu, startup, progress, and finish pages. It
installs for the current Windows user and does not require administrator rights.
The generated `unins000.exe` and the Apps & Features entry provide the standard
uninstall flow. Uninstall stops SenseVoice, removes the HKCU Run value and old
Startup-folder shortcut, and then removes the installed files.

In the standard wizard, clear the `开机自动启动` option to leave startup disabled.

For the portable ZIP only, the legacy script install flow remains available:
  install.cmd
To install the portable ZIP without startup registration:
  install.cmd -NoStartup
  powershell.exe -ExecutionPolicy Bypass -File uninstall.ps1
