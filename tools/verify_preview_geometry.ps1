param(
    [string]$Executable = (Join-Path $PSScriptRoot '..\dist\sensevoice-ui.exe')
)

$ErrorActionPreference = 'Stop'
$Executable = [System.IO.Path]::GetFullPath($Executable)
if (-not (Test-Path -LiteralPath $Executable)) {
    throw "Executable not found: $Executable"
}

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;

public static class SenseVoiceNativeGeometry {
    [StructLayout(LayoutKind.Sequential)]
    public struct Rect {
        public int Left;
        public int Top;
        public int Right;
        public int Bottom;
    }

    [DllImport("user32.dll")]
    public static extern bool GetWindowRect(IntPtr handle, out Rect rect);
}
'@

$workingDirectory = Split-Path -Parent $Executable
$geometryLog = Join-Path ([System.IO.Path]::GetTempPath()) ("sensevoice-geometry-{0}.jsonl" -f [guid]::NewGuid())
$process = Start-Process -FilePath $Executable `
    -ArgumentList @('--preview', '--preview-geometry-test', '--bubble-style', 'ring',
                    '--preview-geometry-log', $geometryLog) `
    -WorkingDirectory $workingDirectory `
    -PassThru
$samples = [System.Collections.Generic.List[object]]::new()

try {
    $deadline = [DateTime]::UtcNow.AddSeconds(4)
    while ([DateTime]::UtcNow -lt $deadline -and -not $process.HasExited) {
        $process.Refresh()
        $handle = $process.MainWindowHandle
        if ($handle -ne [IntPtr]::Zero) {
            $rect = New-Object SenseVoiceNativeGeometry+Rect
            if ([SenseVoiceNativeGeometry]::GetWindowRect($handle, [ref]$rect)) {
                $samples.Add([pscustomobject]@{
                    Left = $rect.Left
                    Top = $rect.Top
                    Width = $rect.Right - $rect.Left
                    Height = $rect.Bottom - $rect.Top
                    Bottom = $rect.Bottom
                })
            }
        }
        Start-Sleep -Milliseconds 25
    }
}
finally {
    if (-not $process.HasExited) {
        Stop-Process -Id $process.Id -Force
    }
}

if (-not (Test-Path -LiteralPath $geometryLog)) {
    throw "No Qt geometry log was written: $geometryLog"
}
$geometrySamples = @(Get-Content -LiteralPath $geometryLog |
    Where-Object { -not [string]::IsNullOrWhiteSpace($_) } |
    ConvertFrom-Json)
Remove-Item -LiteralPath $geometryLog -Force -ErrorAction SilentlyContinue
if ($samples.Count -eq 0 -or $geometrySamples.Count -eq 0) {
    throw 'No preview geometry samples were collected.'
}

$distinctX = @($samples | ForEach-Object { $_.Left } | Sort-Object -Unique)
$distinctWidths = @($samples | ForEach-Object { $_.Width } | Sort-Object -Unique)
$lastBottoms = @($samples | Select-Object -Last 15 | ForEach-Object { $_.Bottom } | Sort-Object -Unique)
$windowCenters = @($geometrySamples | ForEach-Object { [double]$_.window_center_x })
$bubbleCenters = @($geometrySamples | ForEach-Object { [double]$_.bubble_center_x })
$controlCenters = @($geometrySamples | ForEach-Object { [double]$_.control_center_x })
$centerDeltas = @($geometrySamples | ForEach-Object { [math]::Abs([double]$_.center_delta) })
$windowBubbleDeltas = @($geometrySamples | ForEach-Object { [math]::Abs([double]$_.window_bubble_delta) })
$centerRange = ($windowCenters | Measure-Object -Minimum -Maximum)
$maxCenterDelta = ($centerDeltas | Measure-Object -Maximum).Maximum
$maxWindowBubbleDelta = ($windowBubbleDeltas | Measure-Object -Maximum).Maximum
$firstCenter = $windowCenters[0]
$lastCenter = $windowCenters[$windowCenters.Count - 1]

[pscustomobject]@{
    Samples = $samples.Count
    NativeDistinctLeft = $distinctX.Count
    NativeLeftValues = ($distinctX -join ',')
    DistinctWidths = $distinctWidths.Count
    Last15DistinctBottom = $lastBottoms.Count
    GeometrySamples = $geometrySamples.Count
    CenterRangePixels = [math]::Round($centerRange.Maximum - $centerRange.Minimum, 3)
    FirstCenter = [math]::Round($firstCenter, 3)
    LastCenter = [math]::Round($lastCenter, 3)
    MaxBubbleControlCenterDelta = [math]::Round($maxCenterDelta, 3)
    MaxWindowBubbleCenterDelta = [math]::Round($maxWindowBubbleDelta, 3)
} | Format-List

if (($centerRange.Maximum - $centerRange.Minimum) -gt 1.1) {
    $geometrySamples | Select-Object window_center_x, bubble_center_x, control_center_x, center_delta |
        Format-Table -AutoSize
    throw 'Geometry regression: the visual center moved while text grew.'
}
if ($maxCenterDelta -gt 1.1 -or $maxWindowBubbleDelta -gt 1.1) {
    $geometrySamples | Select-Object window_center_x, bubble_center_x, control_center_x, center_delta, window_bubble_delta |
        Format-Table -AutoSize
    throw 'Geometry regression: bubble and control centers are not aligned.'
}
