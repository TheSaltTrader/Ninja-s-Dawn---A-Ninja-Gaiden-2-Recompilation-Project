# Capture a RenderDoc frame from the NG2 intro, unattended.
#
# Two things make this fiddly and are worth keeping:
#
#  * The project path contains spaces ("Ninja Gaiden 2 Xbox360"), and
#    renderdoccmd receives its arguments through Start-Process -ArgumentList,
#    which does not quote array elements reliably - it parsed the working
#    directory as the executable and reported "Failed to launch process".
#    Everything below therefore uses 8.3 short paths, which contain no spaces.
#
#  * RenderDoc's capture key is polled globally by the injected hook, so a real
#    system-level key event triggers it regardless of window focus. That avoids
#    having to find and foreground the game window.
#
#   powershell -ExecutionPolicy Bypass -File tools\rd_capture.ps1 [-CaptureAt 27]

param(
    [int]$CaptureAt = 12,   # seconds after launch - the intro runs ~4s to ~22s, menu from ~25s
    [int]$Tail      = 8,
    [string]$OutSub = "capture",
    [switch]$DumpRegion
)

$ErrorActionPreference = "Stop"

Add-Type @"
using System;
using System.Runtime.InteropServices;
public class RdKey {
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
}
"@

$fso  = New-Object -ComObject Scripting.FileSystemObject
$long = "C:\Users\renoi\ClaudeCode\Ninja Gaiden 2 Xbox360\ng2recomp"
$root = $fso.GetFolder($long).ShortPath
$exe  = $fso.GetFile((Join-Path $long "out\build\win-amd64-Release\ng2.exe")).ShortPath
$rd   = $fso.GetFile((Join-Path $long "tools\renderdoc\RenderDoc_1.46_64\renderdoccmd.exe")).ShortPath

$outDir = Join-Path $long $OutSub
if (-not (Test-Path $outDir)) { New-Item -ItemType Directory -Path $outDir | Out-Null }
$tpl = (New-Object -ComObject Scripting.FileSystemObject).GetFolder($outDir).ShortPath + "\ng2"

Write-Output "root : $root"
Write-Output "exe  : $exe"
Write-Output "tpl  : $tpl"

$args = @("capture", "--working-dir", $root, "--capture-file", $tpl,
          $exe, "--game_data_root", "game")

# The capture point is passed to the game through the environment; the hook in
# diag_hooks.cpp fires the RenderDoc in-application API at that moment. A
# synthetic F12 does NOT reach the injected hook, so do not rely on the key.
$env:REX_NG2_RDOC_AT_MS = ($CaptureAt * 1000)

# Guest plane addresses move between runs, so the capture and the memory dump
# must come from the SAME run: dump a region wide enough to contain the planes,
# then use the capture's own texture-load constants to locate them inside it.
if ($DumpRegion) {
    $env:REX_NG2_PLANE_ADDR  = "0B000000"
    $env:REX_NG2_PLANE_BYTES = "16777216"
    $env:REX_NG2_PLANE_AT_MS = ($CaptureAt * 1000)
    $env:REX_NG2_PLANE_OUT   = "out/region.bin"
}

$p = Start-Process -FilePath $rd -WorkingDirectory $root -PassThru `
        -RedirectStandardOutput (Join-Path $long "out\rd.log") `
        -RedirectStandardError  (Join-Path $long "out\rd.err") `
        -ArgumentList $args

try {
    Start-Sleep -Seconds 18
    $ng = Get-Process ng2 -ErrorAction SilentlyContinue
    if ($ng) {
        $mods = @($ng.Modules | Where-Object { $_.ModuleName -like "*renderdoc*" })
        Write-Output ("ng2 up; renderdoc modules injected: " + $mods.Count)
    } else {
        Write-Output "ng2 NOT running - injection failed, see out\rd.err"
    }

    Write-Output "waiting for the in-app trigger at ${CaptureAt}s"
    Start-Sleep -Seconds ([Math]::Max(1, $CaptureAt - 18) + $Tail)
}
finally {
    # Stop only the processes we started, by PID / by our own image name.
    Get-Process ng2, renderdoccmd -ErrorAction SilentlyContinue | Stop-Process -Force
}

$caps = Get-ChildItem (Join-Path $long $OutSub) -Filter *.rdc -ErrorAction SilentlyContinue
if ($caps) { $caps | ForEach-Object { Write-Output ("CAPTURED " + $_.Name + "  " + [int]($_.Length/1KB) + " KB") } }
else       { Write-Output "no .rdc produced" }
