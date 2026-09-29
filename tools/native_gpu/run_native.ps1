# NG2 native-GPU run launcher - the GUARDED way to run ng2.exe with the transplant (Fable II's run_native.cmd
# and ab_untile_leg.sh, adapted). DO NOT RUN while the user's testing embargo stands (2026-09-26: no NG2 run
# until the Fable session says its optimisation testing is finished).
#
#   powershell -File tools/native_gpu/run_native.ps1 -Pair D:\ng2_frameinterp\ng2-rexglue\out\...  [-Offload]
#              [-Tune "a=b;c=d"] [-Seconds 240] [-Tag lockstep1]
#
# What it does, in order, and why each step exists:
#   1. ONE GAME AT A TIME: refuses if ~/.game-test-lock names a game; claims it with this script's name, an honest
#      `until`, and "kills only its own PID"; releases it on exit.
#   2. STAGES the fork's plugin pair into the run folder with the previous pair backed up and a STAGED.txt marker,
#      so a run that dies before the restore leaves a note naming the backup instead of a silently swapped plugin.
#      Both DLLs come from ONE build (a source-built plugin beside a stock runtime exits at startup, HANDOFF.md).
#      Each staged DLL is verified by CONTENT (RexNgpuSetDrawCallback in the plugin, ng2_uw_mode in both).
#   3. BACKS UP the saves (the run folder's user\ tree) off the run folder and restores them afterwards: running the
#      game writes the user's saves.
#   4. LAUNCHES with Start-Process (NOT WMI - it drops the environment, and NG2_NATIVE_GPU is the switch), records
#      the PID, waits -Seconds, then stops THAT PID only. Never by window title.
#   5. RESTORES the DLL pair, deletes the marker, restores the saves, releases the lock, and prints the log lines
#      that decide the run: [ngpu] exports found / missing, LOCKSTEP totals, the self-check, REGISTER COVERAGE,
#      [swap] guest fps, RINGDUMP, AUDIO BARRIER.
param(
  [Parameter(Mandatory = $true)][string]$Pair,
  [switch]$Offload,
  [string]$Tune = "",
  [int]$Seconds = 240,
  [string]$Tag = "native",
  [int]$SelfCheckEvery = 1024,
  [switch]$Debugger,
  # NG2's own launch-time pad script (ng2_autoskip.cpp, env NG2_PAD_SCRIPT): "t:button,t:button,autoskip:S" -
  # each press at t seconds after launch, held 0.18 s; buttons a b x y start up down left right. No desktop input.
  [string]$PadScript = "",
  # Run windiff2.py against both windows this many seconds after launch (0 = not at all), --pairs 2 for the floor.
  [int]$DiffAt = 0,
  [int]$DiffPairs = 2,
  # The BASELINE arm of a timing pair: the same fork pair, NO native path (NG2_NATIVE_GPU unset). One thing differs.
  [switch]$Baseline,
  # Settings-file overrides for ONE run ("ultrawide=1;texture_pack=1;texture_path=D:\pack"): the settings that have
  # no NG2_* environment override. ng2_settings.cfg is backed up and put back in the restore step, so a scripted leg
  # never changes what the player last saved. The game rewrites the file on exit; the restore wins.
  [string]$Settings = ""
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path))
$bin = Join-Path $root "out\build\win-amd64-Release"
$exe = Join-Path $bin "ng2.exe"
$lockPath = Join-Path $HOME ".game-test-lock"
$stamp = Get-Date -Format "yyyyMMdd_HHmmss"
$backupDlls = Join-Path $bin ("dll_before_" + $Tag + "_" + $stamp)
$saveBackup = "D:\ng2_frameinterp\save_backups\" + $Tag + "_" + $stamp
$marker = Join-Path $bin "STAGED.txt"

function Die($msg) { Write-Host "REFUSED: $msg"; exit 1 }
if (-not (Test-Path $exe)) { Die "no $exe" }
# NG2_NO_PLUGIN=1 (2026-09-27, rexgpu-xenos.dll removed): stage the runtime only, and MOVE any plugin DLL out of the
# run folder for the run (restored after), so the exe is proven to run with no plugin present.
$noPlugin = ($env:NG2_NO_PLUGIN -eq "1")
$needed = if ($noPlugin) { @("rexruntime.dll") } else { @("rexgpu-xenos.dll", "rexruntime.dll") }
foreach ($d in $needed) {
  if (-not (Test-Path (Join-Path $Pair $d))) { Die "no $d in $Pair" }
}
# Content, not origin: the pair must carry the exports and the NG2 features the ledger names.
$plugin = if ($noPlugin) { $null } else { [IO.File]::ReadAllBytes((Join-Path $Pair "rexgpu-xenos.dll")) }
$runtime = [IO.File]::ReadAllBytes((Join-Path $Pair "rexruntime.dll"))
function Has($bytes, $needle) { ([Text.Encoding]::ASCII.GetString($bytes)).Contains($needle) }
if (-not $noPlugin) {
  foreach ($n in @("RexNgpuSetDrawCallback", "RexNgpuSetSwapCallback", "RexNgpuDirtyRegs", "ng2_uw_mode", "solid2d", "pointers reset", "[texpack]")) {
    if (-not (Has $plugin $n)) { Die "the plugin in $Pair lacks '$n'" }
  }
}
foreach ($n in @("ng2_uw_mode", "video_mode_explicit")) {
  if (-not (Has $runtime $n)) { Die "the runtime in $Pair lacks '$n'" }
}

# 1. The lock.
$lock = ""
if (Test-Path $lockPath) { $lock = Get-Content $lockPath -Raw }
if ($lock -and -not ($lock -match "game=none")) { Die "the machine is claimed: $lock" }
$until = (Get-Date).AddSeconds($Seconds + 120).ToString("HH:mm")
$claim = "session=run_native.ps1 game=ng2 until=$until note=native $Tag (offload=$($Offload.IsPresent)), kills only its own PID"
$claim | Out-File -Encoding ascii $lockPath
# Lock race 10:11 (both games ran for ~40 s): two scripts saw the lock free and both wrote. Re-read after a
# settle; if the line is not this claim, someone else won - leave their line alone and refuse.
Start-Sleep -Seconds 3
$again = ""
if (Test-Path $lockPath) { $again = (Get-Content $lockPath -Raw).Trim() }
if ($again -ne $claim) { Die "lost the lock race to: $again" }

$pid_started = 0
$settingsPath = Join-Path $bin "ng2_settings.cfg"
$settingsBackup = $settingsPath + ".before_" + $Tag + "_" + $stamp
try {
  # 1b. Settings overrides for this run (backed up; restored below).
  if ($Settings -and (Test-Path $settingsPath)) {
    Copy-Item $settingsPath $settingsBackup -Force
    $lines = @(Get-Content $settingsPath)
    foreach ($item in ($Settings -split ";")) {
      if (-not $item.Contains("=")) { continue }
      $k = $item.Substring(0, $item.IndexOf("=")).Trim()
      $v = $item.Substring($item.IndexOf("=") + 1)
      $hit = $false
      for ($i = 0; $i -lt $lines.Count; $i++) {
        if ($lines[$i] -match ("^" + [regex]::Escape($k) + "=")) { $lines[$i] = "$k=$v"; $hit = $true }
      }
      if (-not $hit) { $lines += "$k=$v" }
      Write-Host "settings override for this run: $k=$v"
    }
    Set-Content -Path $settingsPath -Value $lines -Encoding ascii
  }

  # 2. Stage the pair.
  New-Item -ItemType Directory -Force $backupDlls | Out-Null
  if (Test-Path (Join-Path $bin "rexgpu-xenos.dll")) { Copy-Item (Join-Path $bin "rexgpu-xenos.dll") $backupDlls -Force }
  Copy-Item (Join-Path $bin "rexruntime.dll") $backupDlls -Force
  "STAGED plugin pair from $Pair by run_native.ps1 at $stamp - the original DLLs are in $backupDlls; copy them back if this file is still here after the game has exited." | Out-File -Encoding ascii $marker
  if ($noPlugin) {
    Remove-Item (Join-Path $bin "rexgpu-xenos.dll") -Force -ErrorAction SilentlyContinue   # its copy is in $backupDlls
    Write-Host "NO PLUGIN: rexgpu-xenos.dll is not in the run folder for this run"
  } else {
    Copy-Item (Join-Path $Pair "rexgpu-xenos.dll") $bin -Force
  }
  Copy-Item (Join-Path $Pair "rexruntime.dll") $bin -Force

  # 3. The saves.
  $userDir = Join-Path $bin "user"
  if (Test-Path $userDir) {
    New-Item -ItemType Directory -Force $saveBackup | Out-Null
    robocopy $userDir (Join-Path $saveBackup "user") /MIR /NFL /NDL /NJH /NJS | Out-Null
    if ($LASTEXITCODE -ge 8) { throw "save backup failed (robocopy $LASTEXITCODE)" }
    Write-Host "saves backed up to $saveBackup"
  }

  # 4. Launch.
  if ($Baseline) {
    Remove-Item Env:NG2_NATIVE_GPU -ErrorAction SilentlyContinue
    Remove-Item Env:NG2_NATIVE_OFFLOAD -ErrorAction SilentlyContinue
  } else {
    $env:NG2_NATIVE_GPU = "1"
    if ($Offload) { $env:NG2_NATIVE_OFFLOAD = "1" } else { Remove-Item Env:NG2_NATIVE_OFFLOAD -ErrorAction SilentlyContinue }
  }
  # PowerShell variable names are case-insensitive: a local `$tune` IS the `-Tune` parameter, and the first
  # leg (17:31) passed "ngpu_backend_selfcheck_every=1;ngpu_backend_selfcheck_every=1" - which the TOML parser
  # refused, dropping EVERY tuning entry for that run. Distinct name, and the text is printed below.
  $tuneText = "ngpu_backend_selfcheck_every=$SelfCheckEvery"
  if ($Tune) { $tuneText = $tuneText + ";" + $Tune }
  $env:NG2_TUNE = $tuneText
  if ($PadScript) { $env:NG2_PAD_SCRIPT = $PadScript } else { Remove-Item Env:NG2_PAD_SCRIPT -ErrorAction SilentlyContinue }
  $logsBefore = @(Get-ChildItem (Join-Path $bin "logs") -Filter "ng2_*.log" -ErrorAction SilentlyContinue | Sort-Object Name)
  if ($Debugger) {
    # Under cdb: on an unhandled exception it prints !analyze and the stacks to $cdbLog and quits, so a crash yields
    # a symbolised stack from ONE run. Symbols: the DLL's pdb beside it, the fork pair's pdbs, the exe's own.
    $cdb = "C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\cdb.exe"
    $cdbLog = "D:\ng2_frameinterp\work\cdb_" + $Tag + "_" + $stamp + ".log"
    $sym = "srv*;" + $bin + ";" + $Pair + ";D:\ng2_frameinterp\ng2-rexglue\out\win-amd64\Release"
    $cmds = '.sympath ' + $sym + ';.reload;g;!analyze -v;~*kv;q'
    $p = Start-Process -FilePath $cdb -ArgumentList @('-g', '-G', '-o', '-logo', $cdbLog, '-y', $sym, '-c', ('"' + $cmds + '"'), ('"' + $exe + '"')) -WorkingDirectory $bin -PassThru
    Write-Host "cdb pid $($p.Id) drives the game; its log: $cdbLog"
    Start-Sleep -Seconds 4
    $game = Get-Process -Name ng2 -ErrorAction SilentlyContinue | Where-Object { $_.StartTime -gt (Get-Date).AddSeconds(-30) } | Select-Object -First 1
    if ($game) { $pid_started = $game.Id } else { $pid_started = $p.Id }
  } else {
    # 2026-09-28 20:36 (user playing Fable II; a leg launched 1 s after their game): a user's own game has no lock
    # line, and every earlier check was seconds old. Re-check at the instant of launch and refuse if ANY game runs.
    $other = Get-Process -Name ng2, fable2 -ErrorAction SilentlyContinue
    if ($other) { Die "a game is running ($(($other | ForEach-Object { "$($_.ProcessName) $($_.Id)" }) -join ', ')) - not launching" }
    $p = Start-Process -FilePath $exe -WorkingDirectory $bin -PassThru
    $pid_started = $p.Id
  }
  Write-Host "started ng2.exe pid $pid_started (offload=$($Offload.IsPresent), NG2_TUNE=$tuneText); running $Seconds s"
  $deadline = (Get-Date).AddSeconds($Seconds)
  $launched = Get-Date
  $diffDone = $false
  while ((Get-Date) -lt $deadline) {
    Start-Sleep -Seconds 5
    if ($p.HasExited) { Write-Host "the game exited on its own (code $($p.ExitCode))"; break }
    if ($DiffAt -gt 0 -and -not $diffDone -and ((Get-Date) - $launched).TotalSeconds -ge $DiffAt) {
      $diffDone = $true
      Write-Host "=== windiff2 at +$DiffAt s (tag $Tag, $DiffPairs pairs) ==="
      python (Join-Path $root "tools\native_gpu\windiff2.py") --tag $Tag --pairs $DiffPairs 2>&1 | ForEach-Object { Write-Host $_ }
    }
  }
  if (-not $p.HasExited) {
    Stop-Process -Id $pid_started -Force -ErrorAction SilentlyContinue
    Write-Host "stopped pid $pid_started"
  }
  # 2026-09-28 (Fable team: an NG2 window on top during their leg): the lock is released only once the game has
  # really gone - a stopped process can take a while to tear down its GPU device and window.
  $gone = $false
  for ($w = 0; $w -lt 60 -and -not $gone; $w++) {
    if (-not (Get-Process -Id $pid_started -ErrorAction SilentlyContinue)) { $gone = $true } else { Start-Sleep -Seconds 1 }
  }
  if (-not $gone) { Write-Host "WARNING: pid $pid_started still present 60 s after stopping" }
  Start-Sleep -Seconds 3
  # The newest log is the run's.
  $log = Get-ChildItem (Join-Path $bin "logs") -Filter "ng2_*.log" -ErrorAction SilentlyContinue | Sort-Object Name | Select-Object -Last 1
  if ($log) {
    Write-Host "=== $($log.FullName) ==="
    Get-Content $log.FullName | Select-String -Pattern "\[ngpu\]|\[ngpu-window\]|\[swap\]|RINGDUMP|AUDIO BARRIER|watchdog|Patch:|REVEAL|self-check|COVERAGE|\[ng2uw\]|\[texpack\]" | Select-Object -Last 60 | ForEach-Object { $_.Line }
  }
}
finally {
  # 5. Restore, always.
  $restored = @()
  foreach ($d in @("rexgpu-xenos.dll", "rexruntime.dll")) {
    if (Test-Path (Join-Path $backupDlls $d)) { Copy-Item (Join-Path $backupDlls $d) $bin -Force; $restored += $d }
  }
  if ($restored.Count -gt 0) {
    Remove-Item $marker -ErrorAction SilentlyContinue
    Write-Host "restored the previous DLLs: $($restored -join ', ')"
  }
  if (Test-Path (Join-Path $saveBackup "user")) {
    robocopy (Join-Path $saveBackup "user") (Join-Path $bin "user") /MIR /NFL /NDL /NJH /NJS | Out-Null
    Write-Host "saves restored from $saveBackup (robocopy $LASTEXITCODE)"
  }
  if (Test-Path $settingsBackup) {
    Copy-Item $settingsBackup $settingsPath -Force
    Remove-Item $settingsBackup -ErrorAction SilentlyContinue
    Write-Host "settings file restored"
  }
  $lockNow = ""
  if (Test-Path $lockPath) { $lockNow = (Get-Content $lockPath -Raw).Trim() }
  if (-not $lockNow -or $lockNow.StartsWith("session=run_native.ps1") -or $lockNow.StartsWith("session=claudecode-0ea6")) {
    "session=run_native.ps1 game=none until=now note=machine-free (native $Tag done)" | Out-File -Encoding ascii $lockPath
  } else {
    Write-Host "lock is not mine at release, left alone: $lockNow"
  }
}
