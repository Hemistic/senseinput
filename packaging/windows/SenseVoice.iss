#pragma codepage 65001
#ifndef AppVersion
#define AppVersion "0.1.0"
#endif
#ifndef FileVersion
#define FileVersion "0.1.0.0"
#endif
#ifndef PackageDir
#define PackageDir "..\..\out\package\SenseVoice-0.1.0-windows-x64"
#endif
#ifndef IconFile
#define IconFile "..\..\resources\sensevoice.ico"
#endif

[Setup]
AppId={{F48C50A8-8C1B-4BF8-BD62-5A74D32F4C7B}
AppName=SenseVoice Desk
AppVersion={#AppVersion}
AppVerName=SenseVoice Desk {#AppVersion}
AppPublisher=SenseVoice Desk
DefaultDirName={localappdata}\SenseVoice
DefaultGroupName=SenseVoice Desk
DisableWelcomePage=no
DisableDirPage=no
DisableProgramGroupPage=no
AllowNoIcons=yes
UsePreviousAppDir=yes
UsePreviousGroup=yes
UsePreviousTasks=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
CloseApplications=yes
AppMutex=Local\SenseVoiceLocalDictation
CloseApplicationsFilter=sensevoice-ui.exe,sensevoice-ui-legacy.exe,sensevoice-stream.exe
RestartApplications=no
Uninstallable=yes
CreateUninstallRegKey=yes
UninstallDisplayName=SenseVoice Desk
UninstallDisplayIcon={app}\sensevoice-ui.exe
SetupIconFile={#IconFile}
OutputBaseFilename=SenseVoice-{#AppVersion}-Setup
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
VersionInfoVersion={#FileVersion}
VersionInfoProductVersion={#FileVersion}
VersionInfoDescription=SenseVoice Desk Setup
VersionInfoProductName=SenseVoice Desk
VersionInfoCopyright=SenseVoice Desk

[Languages]
Name: "chinesesimplified"; MessagesFile: "compiler:Languages\ChineseSimplified.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "autostart"; Description: "开机自动启动"; GroupDescription: "启动选项:"; Flags: checkedonce

[Files]
Source: "{#PackageDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs; Excludes: "install.ps1,install.cmd,uninstall.ps1,README.txt"
Source: "{#PackageDir}\uninstall.ps1"; DestDir: "{app}"; Flags: ignoreversion; Attribs: hidden

[Icons]
Name: "{group}\SenseVoice Desk"; Filename: "{app}\sensevoice-ui.exe"; WorkingDir: "{app}"; IconFilename: "{app}\sensevoice-ui.exe"; Comment: "SenseVoice local voice input"
Name: "{group}\Uninstall SenseVoice Desk"; Filename: "{uninstallexe}"

[Registry]
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "SenseVoice"; ValueData: """{app}\sensevoice-ui.exe"""; Flags: uninsdeletevalue; Tasks: autostart
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: none; ValueName: "SenseVoice"; Flags: deletevalue; Tasks: not autostart
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Uninstall\SenseVoice"; Flags: deletekey

[InstallDelete]
Type: files; Name: "{app}\install.ps1"
Type: files; Name: "{app}\install.cmd"
Type: files; Name: "{app}\uninstall.ps1"
Type: files; Name: "{userappdata}\Microsoft\Windows\Start Menu\Programs\Startup\SenseVoice.lnk"

[Run]
Filename: "{app}\sensevoice-ui.exe"; Description: "启动 SenseVoice Desk"; Flags: nowait postinstall skipifsilent

[UninstallRun]
Filename: "{sys}\WindowsPowerShell\v1.0\powershell.exe"; Parameters: "-NoProfile -NonInteractive -ExecutionPolicy Bypass -File ""{app}\uninstall.ps1"" -Quiet -KeepFiles"; Flags: runhidden waituntilterminated; RunOnceId: "StopSenseVoice"

[UninstallDelete]
Type: files; Name: "{userappdata}\Microsoft\Windows\Start Menu\Programs\Startup\SenseVoice.lnk"
Type: filesandordirs; Name: "{app}"
