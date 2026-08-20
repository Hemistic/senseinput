param(
    [string]$Configuration = 'Release',
    [string]$Version = '0.1.0',
    [switch]$SkipBuild
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$buildDirectory = Join-Path $root 'build'
$binaryDirectory = Join-Path $buildDirectory $Configuration
$distDirectory = Join-Path $root 'dist'
$stagingRoot = Join-Path $root 'out\package'
$packageDirectory = Join-Path $stagingRoot "SenseVoice-$Version-windows-x64"
$archivePath = Join-Path $root "out\SenseVoice-$Version-windows-x64.zip"
$cmake = 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'

if (-not $SkipBuild) {
    & $cmake --build $buildDirectory --config $Configuration --target sensevoice-ui sensevoice-ui-legacy sensevoice-stream sensevoice-inject --parallel 8
    if ($LASTEXITCODE -ne 0) { throw 'Release build failed.' }
}

if (-not (Test-Path -LiteralPath $distDirectory)) { throw "Missing dist directory: $distDirectory" }
foreach ($executableName in @('sensevoice-ui.exe', 'sensevoice-ui-legacy.exe', 'sensevoice-stream.exe')) {
    $builtExecutable = Join-Path $binaryDirectory $executableName
    if (-not (Test-Path -LiteralPath $builtExecutable)) {
        throw "Missing built executable: $builtExecutable"
    }
    Copy-Item -LiteralPath $builtExecutable -Destination (Join-Path $distDirectory $executableName) -Force
}
Remove-Item -LiteralPath $packageDirectory -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $packageDirectory | Out-Null
Get-ChildItem -LiteralPath $distDirectory -Force |
    Where-Object { $_.Name -notmatch '\.png$' -and $_.Name -notin @('使用说明.txt', 'preview-ui.exe') } |
    ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination $packageDirectory -Recurse -Force }
Copy-Item -LiteralPath (Join-Path $binaryDirectory 'sensevoice-ui.exe') -Destination $packageDirectory -Force
Copy-Item -LiteralPath (Join-Path $binaryDirectory 'sensevoice-ui-legacy.exe') -Destination $packageDirectory -Force
Copy-Item -LiteralPath (Join-Path $binaryDirectory 'sensevoice-stream.exe') -Destination $packageDirectory -Force
Copy-Item -LiteralPath (Join-Path $binaryDirectory 'sensevoice-inject.exe') -Destination $packageDirectory -Force
Copy-Item -LiteralPath (Join-Path $root 'packaging\windows\install.ps1') -Destination $packageDirectory -Force
Copy-Item -LiteralPath (Join-Path $root 'packaging\windows\install.cmd') -Destination $packageDirectory -Force
Copy-Item -LiteralPath (Join-Path $root 'packaging\windows\uninstall.ps1') -Destination $packageDirectory -Force
Copy-Item -LiteralPath (Join-Path $root 'packaging\windows\README.txt') -Destination $packageDirectory -Force

New-Item -ItemType Directory -Force -Path (Split-Path -Parent $archivePath) | Out-Null
Remove-Item -LiteralPath $archivePath -Force -ErrorAction SilentlyContinue
Compress-Archive -Path (Join-Path $packageDirectory '*') -DestinationPath $archivePath -CompressionLevel Optimal
Write-Output "Package: $archivePath"
