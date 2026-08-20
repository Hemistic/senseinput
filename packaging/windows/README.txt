SenseVoice Windows package

Run install.cmd to install the application for the current Windows user.
The installer creates a Start menu shortcut and registers SenseVoice in the
current user's Startup folder. No administrator rights are required.

To install without startup registration:
  install.cmd -NoStartup

The installed application can be removed from Apps and Features or by running:
  powershell.exe -ExecutionPolicy Bypass -File uninstall.ps1
