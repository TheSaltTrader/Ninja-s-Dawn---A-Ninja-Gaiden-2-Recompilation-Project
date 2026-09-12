# Assemble a self-contained, pre-configured NG2 folder that can be moved to
# another PC as it is. Internal use: it carries the game, so it never leaves
# this machine except by hand.
#
#   powershell -File scratchpad\make_portable.ps1 -Dest "D:\Ninja Gaiden 2 Portable" -Release v1.0.9
#
# Layout (everything the app looks for is beside ng2.exe, by its own defaults):
#   ng2.exe, rexruntime.dll, rexgpu-xenos.dll, the VC runtime, gamecontrollerdb.txt, tools\
#   game\        the extracted disc (default.xex + data)   <- game_path empty
#   dlc\         the STFS DLC packages                       <- dlc_path empty
#   user\        profile, saves, achievements, cache
#   textures\    dump\ (raw guest textures) + pack\ (the upscaled pack)  <- texture_path=textures
#   iso\         the disc image, kept for re-extraction
#   ng2_settings.cfg  the current settings with the paths above
param(
  [string]$Dest = "D:\Ninja Gaiden 2 Portable",
  [string]$Release = "v1.0.9"
)
$ErrorActionPreference = "Stop"
$base = "C:\Users\renoi\ClaudeCode\Ninja Gaiden 2 Xbox360"
$rel  = "$base\Releases\$Release"
$src = @{
  release  = $rel
  game     = "$base\ng2recomp\game"
  dlc      = "$base\DLC"
  user     = "$base\ng2recomp\out\build\win-amd64-Release\user"
  dump     = "C:\ng2tex\dump"
  pack     = "C:\ng2tex\pack"
  iso      = "$base\ISO"
  cfg      = "$base\ng2recomp\out\build\win-amd64-Release\ng2_settings.cfg"
}
foreach ($k in $src.Keys) { if (-not (Test-Path $src[$k])) { throw "missing source $k : $($src[$k])" } }
if (-not (Test-Path "$rel\ng2.exe")) { throw "no ng2.exe in $rel" }

New-Item -ItemType Directory -Force $Dest | Out-Null
function Copy-Tree($from, $to, $label) {
  $t0 = Get-Date
  New-Item -ItemType Directory -Force $to | Out-Null
  $r = & robocopy $from $to /E /MT:16 /R:2 /W:2 /NFL /NDL /NJH /NJS /NP /XF "PUT_FILES_HERE.txt"
  $code = $LASTEXITCODE   # robocopy: 0-7 = success classes, 8+ = failures
  if ($code -ge 8) { throw "robocopy failed ($code) copying $label" }
  $n = (Get-ChildItem $to -Recurse -File | Measure-Object).Count
  $gb = (Get-ChildItem $to -Recurse -File | Measure-Object Length -Sum).Sum / 1GB
  "{0,-10} {1,7} files {2,8:N2} GB  {3,5:N0} s" -f $label, $n, $gb, ((Get-Date) - $t0).TotalSeconds
}
Copy-Tree $src.release  $Dest              "app"
Copy-Tree $src.game     "$Dest\game"       "game"
Copy-Tree $src.dlc      "$Dest\dlc"        "dlc"
Copy-Tree $src.user     "$Dest\user"       "user"
Copy-Tree $src.dump     "$Dest\textures\dump" "dump"
Copy-Tree $src.pack     "$Dest\textures\pack" "pack"
# The AI upscaler (Real-ESRGAN) lives beside dump/ and pack/; without it the
# other PC falls back to Lanczos and the menu offers a download.
if (Test-Path "C:\ng2tex\upscaler") { Copy-Tree "C:\ng2tex\upscaler" "$Dest\textures\upscaler" "upscaler" }
Copy-Tree $src.iso      "$Dest\iso"        "iso"

# The settings: this machine's, with every path made relative to the folder.
$cfg = Get-Content $src.cfg
$cfg = $cfg -replace '^texture_path=.*$', 'texture_path=textures'
$cfg = $cfg -replace '^game_path=.*$',    'game_path='
$cfg = $cfg -replace '^dlc_path=.*$',     'dlc_path=dlc'
$cfg = $cfg -replace '^iso_path=.*$',     'iso_path=iso\Ninja Gaiden II.iso'
$cfg = $cfg -replace '^configured=.*$',   'configured=1'
Set-Content -Encoding ascii "$Dest\ng2_settings.cfg" $cfg
"settings written (paths relative: game\, dlc\, textures\, iso\)"

@"
Ninja's Dawn - portable folder ($Release)
=========================================
Everything is beside ng2.exe: the extracted disc (game\), the DLC (dlc\), the
profile and saves (user\), the raw texture dump and the upscaled pack
(textures\dump, textures\pack), and the disc image (iso\). The settings file
points at those folders by relative path, so the folder can be moved to any
drive or PC as it is. Run ng2.exe. Hold Shift at launch for the setup screen.

Not for distribution: game\ and iso\ are the game itself.
"@ | Set-Content -Encoding ascii "$Dest\README-PORTABLE.txt"

"--- verification: source vs copy (files / bytes)"
foreach ($pair in @(@("game",$src.game,"$Dest\game"), @("dlc",$src.dlc,"$Dest\dlc"), @("user",$src.user,"$Dest\user"),
                    @("dump",$src.dump,"$Dest\textures\dump"), @("pack",$src.pack,"$Dest\textures\pack"), @("iso",$src.iso,"$Dest\iso"))) {
  $a = Get-ChildItem $pair[1] -Recurse -File | Where-Object { $_.Name -ne "PUT_FILES_HERE.txt" } | Measure-Object Length -Sum
  $b = Get-ChildItem $pair[2] -Recurse -File | Measure-Object Length -Sum
  $ok = ($a.Count -eq $b.Count) -and ($a.Sum -eq $b.Sum)
  "{0,-6} {1,7}/{2,-7} files  {3,14:N0}/{4,-14:N0} bytes  {5}" -f $pair[0], $a.Count, $b.Count, $a.Sum, $b.Sum, $(if ($ok) { "OK" } else { "MISMATCH" })
}
"--- total"
$t = Get-ChildItem $Dest -Recurse -File | Measure-Object Length -Sum
"{0} files, {1:N1} GB in {2}" -f $t.Count, ($t.Sum/1GB), $Dest
