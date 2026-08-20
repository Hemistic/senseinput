param(
    [string]$InstallDirectory = (Join-Path $env:LOCALAPPDATA 'SenseVoice'),
    [switch]$NoStartup
)

$ErrorActionPreference = 'Stop'
$packageRoot = $PSScriptRoot
$InstallDirectory = [System.IO.Path]::GetFullPath($InstallDirectory)
$executable = Join-Path $InstallDirectory 'sensevoice-ui.exe'
$startupDirectory = Join-Path $env:APPDATA 'Microsoft\Windows\Start Menu\Programs\Startup'
$startMenuDirectory = Join-Path $env:APPDATA 'Microsoft\Windows\Start Menu\Programs\SenseVoice'
$startupShortcut = Join-Path $startupDirectory 'SenseVoice.lnk'
$startMenuShortcut = Join-Path $startMenuDirectory 'SenseVoice.lnk'
$runKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'
$runValueName = 'SenseVoice'

# The single-file bootstrap uses this name for its temporary archive; never leave it installed.
Remove-Item -LiteralPath (Join-Path $InstallDirectory 'payload.zip') -Force -ErrorAction SilentlyContinue

New-Item -ItemType Directory -Force -Path $InstallDirectory | Out-Null
Get-ChildItem -LiteralPath $packageRoot -Force |
    Where-Object { $_.Name -notin @('install.ps1', 'install.cmd', 'payload.zip') } |
    ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination $InstallDirectory -Recurse -Force }

function New-SenseVoiceShortcut([string]$path) {
    $shell = New-Object -ComObject WScript.Shell
    $shortcut = $shell.CreateShortcut($path)
    $shortcut.TargetPath = $executable
    $shortcut.WorkingDirectory = $InstallDirectory
    $shortcut.IconLocation = "$executable,0"
    $shortcut.Description = 'SenseVoice local voice input'
    $shortcut.Save()
}

New-Item -ItemType Directory -Force -Path $startMenuDirectory | Out-Null
New-SenseVoiceShortcut $startMenuShortcut
New-Item -Path $runKey -Force | Out-Null
if ($NoStartup) {
    Remove-ItemProperty -Path $runKey -Name $runValueName -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $startupShortcut -Force -ErrorAction SilentlyContinue
} else {
    # Register one standard per-user Run entry. The old Startup-folder link is
    # removed so upgrades never launch duplicate instances.
    New-ItemProperty -Path $runKey -Name $runValueName -Value "`"$executable`"" -PropertyType String -Force | Out-Null
    Remove-Item -LiteralPath $startupShortcut -Force -ErrorAction SilentlyContinue
}

$uninstallKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\SenseVoice'
New-Item -Path $uninstallKey -Force | Out-Null
New-ItemProperty -Path $uninstallKey -Name DisplayName -Value 'SenseVoice Local Voice Input' -PropertyType String -Force | Out-Null
New-ItemProperty -Path $uninstallKey -Name InstallLocation -Value $InstallDirectory -PropertyType String -Force | Out-Null
New-ItemProperty -Path $uninstallKey -Name DisplayIcon -Value "$executable,0" -PropertyType String -Force | Out-Null
New-ItemProperty -Path $uninstallKey -Name UninstallString -Value "powershell.exe -ExecutionPolicy Bypass -File `"$(Join-Path $InstallDirectory 'uninstall.ps1')`"" -PropertyType String -Force | Out-Null
New-ItemProperty -Path $uninstallKey -Name QuietUninstallString -Value "powershell.exe -NoProfile -ExecutionPolicy Bypass -File `"$(Join-Path $InstallDirectory 'uninstall.ps1')`" -Quiet" -PropertyType String -Force | Out-Null
New-ItemProperty -Path $uninstallKey -Name Publisher -Value 'SenseVoice Desk' -PropertyType String -Force | Out-Null
New-ItemProperty -Path $uninstallKey -Name DisplayVersion -Value '0.1.0' -PropertyType String -Force | Out-Null
New-ItemProperty -Path $uninstallKey -Name NoModify -Value 1 -PropertyType DWord -Force | Out-Null
New-ItemProperty -Path $uninstallKey -Name NoRepair -Value 1 -PropertyType DWord -Force | Out-Null

Write-Output "Installed to $InstallDirectory"
if ($NoStartup) { Write-Output 'Startup registration: disabled' }
else { Write-Output "Startup registration: HKCU\Software\Microsoft\Windows\CurrentVersion\Run\$runValueName" }
