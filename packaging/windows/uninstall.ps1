param(
    [string]$InstallDirectory = $PSScriptRoot,
    [switch]$Quiet
)

$ErrorActionPreference = 'Stop'
$startupShortcut = Join-Path $env:APPDATA 'Microsoft\Windows\Start Menu\Programs\Startup\SenseVoice.lnk'
$startMenuShortcut = Join-Path $env:APPDATA 'Microsoft\Windows\Start Menu\Programs\SenseVoice\SenseVoice.lnk'
$startMenuDirectory = Split-Path -Parent $startMenuShortcut
$uninstallKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\SenseVoice'
$runKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'
$runValueName = 'SenseVoice'

# The uninstaller is launched from Apps & Features while the tray process may
# still be running. Stop only this application's processes so files and the
# startup entry can be removed deterministically.
Get-Process -Name 'sensevoice-ui' -ErrorAction SilentlyContinue |
    Where-Object { $_.Path -eq (Join-Path ([System.IO.Path]::GetFullPath($InstallDirectory)) 'sensevoice-ui.exe') } |
    Stop-Process -Force -ErrorAction SilentlyContinue

Remove-Item -LiteralPath $startupShortcut -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath $startMenuShortcut -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath $startMenuDirectory -Force -Recurse -ErrorAction SilentlyContinue
Remove-ItemProperty -Path $runKey -Name $runValueName -Force -ErrorAction SilentlyContinue
Remove-Item -Path $uninstallKey -Force -Recurse -ErrorAction SilentlyContinue

$resolvedInstallDirectory = [System.IO.Path]::GetFullPath($InstallDirectory)
if ((Split-Path -Leaf $resolvedInstallDirectory) -eq 'SenseVoice' -and
    (Split-Path -Parent $resolvedInstallDirectory) -eq $env:LOCALAPPDATA) {
    Remove-Item -LiteralPath $resolvedInstallDirectory -Force -Recurse -ErrorAction SilentlyContinue
}

if (-not $Quiet) { Write-Output 'SenseVoice was uninstalled.' }
