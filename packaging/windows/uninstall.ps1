param(
    [string]$InstallDirectory = $PSScriptRoot
)

$ErrorActionPreference = 'Stop'
$startupShortcut = Join-Path $env:APPDATA 'Microsoft\Windows\Start Menu\Programs\Startup\SenseVoice.lnk'
$startMenuShortcut = Join-Path $env:APPDATA 'Microsoft\Windows\Start Menu\Programs\SenseVoice\SenseVoice.lnk'
$startMenuDirectory = Split-Path -Parent $startMenuShortcut
$uninstallKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\SenseVoice'

Remove-Item -LiteralPath $startupShortcut -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath $startMenuShortcut -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath $startMenuDirectory -Force -Recurse -ErrorAction SilentlyContinue
Remove-Item -Path $uninstallKey -Force -Recurse -ErrorAction SilentlyContinue

$resolvedInstallDirectory = [System.IO.Path]::GetFullPath($InstallDirectory)
if ((Split-Path -Leaf $resolvedInstallDirectory) -eq 'SenseVoice' -and
    (Split-Path -Parent $resolvedInstallDirectory) -eq $env:LOCALAPPDATA) {
    Remove-Item -LiteralPath $resolvedInstallDirectory -Force -Recurse -ErrorAction SilentlyContinue
}

Write-Output 'SenseVoice was uninstalled.'
