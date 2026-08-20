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
$process = Start-Process -FilePath $Executable `
    -ArgumentList @('--preview', '--preview-geometry-test', '--bubble-style', 'ring') `
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

if ($samples.Count -eq 0) {
    throw 'No preview window samples were collected.'
}

$distinctX = @($samples | ForEach-Object { $_.Left } | Sort-Object -Unique)
$distinctWidths = @($samples | ForEach-Object { $_.Width } | Sort-Object -Unique)
$lastBottoms = @($samples | Select-Object -Last 15 | ForEach-Object { $_.Bottom } | Sort-Object -Unique)

[pscustomobject]@{
    Samples = $samples.Count
    DistinctX = $distinctX.Count
    XValues = ($distinctX -join ',')
    DistinctWidths = $distinctWidths.Count
    Last15DistinctBottom = $lastBottoms.Count
} | Format-List

if ($distinctX.Count -ne 1) {
    $samples | Format-Table -AutoSize
    throw 'Geometry regression: the preview window x coordinate moved.'
}
