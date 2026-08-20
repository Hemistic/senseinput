SenseVoice Windows package

Run install.cmd to install the application for the current Windows user.
The installer creates a Start menu shortcut and registers SenseVoice in the
current user's HKCU Run key. No administrator rights are required.

To install without startup registration:
  install.cmd -NoStartup

The installed application can be removed from Apps and Features or by running:
The uninstaller stops the installed process and clears the Run entry and
shortcuts:
  powershell.exe -ExecutionPolicy Bypass -File uninstall.ps1
