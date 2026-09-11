# Launch ng2, let it settle, and photograph its window.
#
#   powershell -ExecutionPolicy Bypass -File tools\screenshot.ps1 [-Seconds 20] [-Out out\shot.png]
#
# Uses PrintWindow with PW_RENDERFULLCONTENT, which asks the window to render
# itself into an offscreen DC. That is the only safe option here: capturing the
# screen region under the window instead will photograph whatever is actually
# on top of it, which is not necessarily this game and may be anything at all.
# Do not "fix" a blank capture by falling back to CopyFromScreen.
#
# The process is always stopped by PID, never by window title.

param(
    [int]$Seconds = 20,
    [string]$Out = "out\screenshot.png",
    [string]$Config = "Release"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root "out\build\win-amd64-$Config\ng2.exe"
if (-not (Test-Path $exe)) { throw "not built: $exe" }

Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class Win {
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
}
"@

# Relative paths + WorkingDirectory: the project path contains spaces and
# Start-Process does not quote array arguments reliably.
$p = Start-Process -FilePath $exe -WorkingDirectory $root -ArgumentList @(
    "--game_data_root", "game",
    "--log_file", "out\screenshot.log",
    "--log_level", "debug") -PassThru

try {
    Start-Sleep -Seconds $Seconds
    if ($p.HasExited) { throw "process exited early with code $($p.ExitCode)" }

    $p.Refresh()
    $h = $p.MainWindowHandle
    if ($h -eq [IntPtr]::Zero) { throw "no main window" }

    $r = New-Object Win+RECT
    [void][Win]::GetWindowRect($h, [ref]$r)
    $w = $r.R - $r.L
    $ht = $r.B - $r.T
    Write-Output "window '$($p.MainWindowTitle)' size ${w}x${ht}"

    $bmp = New-Object System.Drawing.Bitmap $w, $ht
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $dc = $g.GetHdc()
    $ok = [Win]::PrintWindow($h, $dc, 2)   # PW_RENDERFULLCONTENT
    $g.ReleaseHdc($dc)
    $g.Dispose()

    # Report whether anything was actually drawn, rather than saving a lie.
    $distinct = @{}
    for ($y = 0; $y -lt $ht; $y += [Math]::Max(1, [int]($ht / 40))) {
        for ($x = 0; $x -lt $w; $x += [Math]::Max(1, [int]($w / 40))) {
            $distinct[$bmp.GetPixel($x, $y).ToArgb()] = $true
        }
    }
    Write-Output "PrintWindow ok=$ok, distinct sampled colours=$($distinct.Count)"

    $path = Join-Path $root $Out
    $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    Write-Output "saved $path"
}
finally {
    if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
}
