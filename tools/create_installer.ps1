param(
    [string]$Version = '0.1.0',
    [switch]$SkipPackage,
    [string]$CompilerPath = ''
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$packageScript = Join-Path $root 'tools\package_windows.ps1'
$packageDirectory = Join-Path $root ('out\package\SenseVoice-{0}-windows-x64' -f $Version)
$outputDirectory = Join-Path $root 'out'
$issPath = Join-Path $root 'packaging\windows\SenseVoice.iss'
$iconPath = Join-Path $root 'resources\sensevoice.ico'
$installerPath = Join-Path $outputDirectory ('SenseVoice-{0}-Setup.exe' -f $Version)

function Resolve-InnoCompiler {
    param([string]$RequestedPath)

    $candidates = @()
    if (-not [string]::IsNullOrWhiteSpace($RequestedPath)) { $candidates += $RequestedPath }
    if (-not [string]::IsNullOrWhiteSpace($env:INNO_SETUP_HOME)) {
        $candidates += (Join-Path $env:INNO_SETUP_HOME 'ISCC.exe')
    }
    $programFilesX86 = ${env:ProgramFiles(x86)}
    $candidates += @(
        (Join-Path $root '.tools\InnoSetup\ISCC.exe'),
        (Join-Path $root '.tools\InnoSetup7\ISCC.exe'),
        (Join-Path $programFilesX86 'Inno Setup 7\ISCC.exe'),
        (Join-Path $programFilesX86 'Inno Setup 6\ISCC.exe'),
        (Join-Path $env:ProgramFiles 'Inno Setup 7\ISCC.exe'),
        (Join-Path $env:ProgramFiles 'Inno Setup 6\ISCC.exe')
    )
    foreach ($candidate in $candidates) {
        if (-not [string]::IsNullOrWhiteSpace($candidate) -and (Test-Path -LiteralPath $candidate)) {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }
    $command = Get-Command ISCC.exe -ErrorAction SilentlyContinue
    if ($null -ne $command) { return $command.Source }
    throw 'Inno Setup compiler (ISCC.exe) was not found. Install Inno Setup 6/7, set INNO_SETUP_HOME, or pass -CompilerPath.'
}

if (-not (Test-Path -LiteralPath $issPath)) { throw "Installer definition not found: $issPath" }
if (-not (Test-Path -LiteralPath $iconPath)) { throw "Installer icon not found: $iconPath" }

if (-not $SkipPackage) {
    & $packageScript -Version $Version
    if ($LASTEXITCODE -ne 0) { throw 'Package creation failed.' }
}
if (-not (Test-Path -LiteralPath $packageDirectory)) {
    throw "Package staging directory not found: $packageDirectory. Run package_windows.ps1 first."
}

$iscc = Resolve-InnoCompiler -RequestedPath $CompilerPath
New-Item -ItemType Directory -Force -Path $outputDirectory | Out-Null
Remove-Item -LiteralPath $installerPath -Force -ErrorAction SilentlyContinue

$arguments = @(
    $issPath,
    "/DAppVersion=$Version",
    "/DPackageDir=$packageDirectory",
    "/DIconFile=$iconPath",
    ("/DFileVersion={0}.0" -f $Version),
    "/O$outputDirectory"
)
& $iscc @arguments
if ($LASTEXITCODE -ne 0) { throw "Inno Setup compilation failed with exit code $LASTEXITCODE." }
if (-not (Test-Path -LiteralPath $installerPath)) { throw "Installer was not generated: $installerPath" }

$result = Get-Item -LiteralPath $installerPath
Write-Output "Compiler: $iscc"
Write-Output "Installer: $($result.FullName)"
Write-Output "Size: $($result.Length) bytes"
