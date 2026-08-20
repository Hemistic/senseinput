param(
    [string]$InstallDirectory = $PSScriptRoot,
    [switch]$Quiet,
    [switch]$KeepFiles
)

$ErrorActionPreference = 'Stop'
$startupShortcut = Join-Path $env:APPDATA 'Microsoft\Windows\Start Menu\Programs\Startup\SenseVoice.lnk'
$startMenuShortcut = Join-Path $env:APPDATA 'Microsoft\Windows\Start Menu\Programs\SenseVoice\SenseVoice.lnk'
$startMenuDirectory = Split-Path -Parent $startMenuShortcut
$uninstallKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\SenseVoice'
$runKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'
$runValueName = 'SenseVoice'

# The standard Inno uninstaller calls this hidden helper before deleting files.
# Match executable paths so a portable or different-version instance is not
# terminated accidentally.
$resolvedInstallDirectory = [System.IO.Path]::GetFullPath($InstallDirectory)
$executableNames = @('sensevoice-ui', 'sensevoice-ui-legacy', 'sensevoice-stream')
foreach ($executableName in $executableNames) {
    Get-Process -Name $executableName -ErrorAction SilentlyContinue |
        Where-Object { $_.Path -eq (Join-Path $resolvedInstallDirectory ($executableName + '.exe')) } |
        Stop-Process -Force -ErrorAction SilentlyContinue
}

Remove-Item -LiteralPath $startupShortcut -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath $startMenuShortcut -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath $startMenuDirectory -Force -Recurse -ErrorAction SilentlyContinue
Remove-ItemProperty -Path $runKey -Name $runValueName -Force -ErrorAction SilentlyContinue
Remove-Item -Path $uninstallKey -Force -Recurse -ErrorAction SilentlyContinue

if (-not $KeepFiles -and (Split-Path -Leaf $resolvedInstallDirectory) -eq 'SenseVoice' -and
    (Split-Path -Parent $resolvedInstallDirectory) -eq $env:LOCALAPPDATA) {
    Remove-Item -LiteralPath $resolvedInstallDirectory -Force -Recurse -ErrorAction SilentlyContinue
}

if (-not $Quiet) { Write-Output 'SenseVoice was uninstalled.' }
