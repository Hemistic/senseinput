param(
    [string]$Version = '0.1.0',
    [switch]$SkipPackage
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$packageScript = Join-Path $root 'tools\package_windows.ps1'
$packageArchive = Join-Path $root "out\SenseVoice-$Version-windows-x64.zip"
$outputDirectory = Join-Path $root 'out'
$buildDirectory = Join-Path $root 'build'
$stubSourcePath = Join-Path $root 'build\Release\sensevoice-setup-stub.exe'
$installerPath = Join-Path $outputDirectory "SenseVoice-$Version-Setup.exe"
$legacySedPath = Join-Path $outputDirectory "SenseVoice-$Version-Setup.sed"
$legacySourceDirectory = Join-Path $outputDirectory 'installer-source'
$cmake = 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'

if (-not $SkipPackage) {
    & $packageScript -Version $Version
    if ($LASTEXITCODE -ne 0) { throw 'Package creation failed.' }
}
if (-not (Test-Path -LiteralPath $packageArchive)) {
    throw "Package archive not found: $packageArchive"
}

& $cmake --build $buildDirectory --config Release --target sensevoice-setup-stub --parallel 8
if ($LASTEXITCODE -ne 0) { throw 'Installer stub build failed.' }
if (-not (Test-Path -LiteralPath $stubSourcePath)) {
    throw "Installer stub not found: $stubSourcePath"
}

New-Item -ItemType Directory -Force -Path $outputDirectory | Out-Null
$stubInfo = Get-Item -LiteralPath $stubSourcePath
$archiveInfo = Get-Item -LiteralPath $packageArchive
Remove-Item -LiteralPath $installerPath -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath $legacySedPath -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath $legacySourceDirectory -Recurse -Force -ErrorAction SilentlyContinue

$output = [System.IO.File]::Open($installerPath, [System.IO.FileMode]::CreateNew, [System.IO.FileAccess]::Write, [System.IO.FileShare]::Read)
try {
    $stubStream = [System.IO.File]::OpenRead($stubSourcePath)
    try { $stubStream.CopyTo($output) } finally { $stubStream.Dispose() }
    $payloadOffset = [int64]$stubInfo.Length

    $archiveStream = [System.IO.File]::OpenRead($packageArchive)
    try { $archiveStream.CopyTo($output) } finally { $archiveStream.Dispose() }

    $writer = [System.IO.BinaryWriter]::new($output, [System.Text.Encoding]::ASCII, $true)
    try {
        $magic = New-Object byte[] 20
        $magicText = [System.Text.Encoding]::ASCII.GetBytes('SENSEVOICE_SETUP_V1')
        [System.Array]::Copy($magicText, 0, $magic, 0, $magicText.Length)
        $writer.Write($magic)
        $writer.Write([int64]$payloadOffset)
        $writer.Write([int64]$archiveInfo.Length)
    } finally { $writer.Dispose() }
} finally {
    $output.Dispose()
}

$result = Get-Item -LiteralPath $installerPath
Write-Output "Installer: $($result.FullName)"
Write-Output "Size: $($result.Length) bytes"
Write-Output "Payload: offset=$payloadOffset size=$($archiveInfo.Length)"
