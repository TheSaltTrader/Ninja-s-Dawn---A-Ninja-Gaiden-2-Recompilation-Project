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
  [int]$SelfCheckEvery = 1024
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
foreach ($d in @("rexgpu-xenos.dll", "rexruntime.dll")) {
  if (-not (Test-Path (Join-Path $Pair $d))) { Die "no $d in $Pair" }
}
# Content, not origin: the pair must carry the exports and the NG2 features the ledger names.
$plugin = [IO.File]::ReadAllBytes((Join-Path $Pair "rexgpu-xenos.dll"))
$runtime = [IO.File]::ReadAllBytes((Join-Path $Pair "rexruntime.dll"))
function Has($bytes, $needle) { ([Text.Encoding]::ASCII.GetString($bytes)).Contains($needle) }
foreach ($n in @("RexNgpuSetDrawCallback", "RexNgpuSetSwapCallback", "RexNgpuDirtyRegs", "ng2_uw_mode", "solid2d", "pointers reset", "[texpack]")) {
  if (-not (Has $plugin $n)) { Die "the plugin in $Pair lacks '$n'" }
}
foreach ($n in @("ng2_uw_mode", "video_mode_explicit")) {
  if (-not (Has $runtime $n)) { Die "the runtime in $Pair lacks '$n'" }
}

# 1. The lock.
$lock = ""
if (Test-Path $lockPath) { $lock = Get-Content $lockPath -Raw }
if ($lock -and -not ($lock -match "game=none")) { Die "the machine is claimed: $lock" }
$until = (Get-Date).AddSeconds($Seconds + 120).ToString("HH:mm")
"session=run_native.ps1 game=ng2 until=$until note=native $Tag (offload=$($Offload.IsPresent)), kills only its own PID" | Out-File -Encoding ascii $lockPath

$pid_started = 0
try {
  # 2. Stage the pair.
  New-Item -ItemType Directory -Force $backupDlls | Out-Null
  Copy-Item (Join-Path $bin "rexgpu-xenos.dll") $backupDlls -Force
  Copy-Item (Join-Path $bin "rexruntime.dll") $backupDlls -Force
  "STAGED plugin pair from $Pair by run_native.ps1 at $stamp - the original DLLs are in $backupDlls; copy them back if this file is still here after the game has exited." | Out-File -Encoding ascii $marker
  Copy-Item (Join-Path $Pair "rexgpu-xenos.dll") $bin -Force
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
  $env:NG2_NATIVE_GPU = "1"
  if ($Offload) { $env:NG2_NATIVE_OFFLOAD = "1" } else { Remove-Item Env:NG2_NATIVE_OFFLOAD -ErrorAction SilentlyContinue }
  $tune = "ngpu_backend_selfcheck_every=$SelfCheckEvery"
  if ($Tune) { $tune = $tune + ";" + $Tune }
  $env:NG2_TUNE = $tune
  $logsBefore = @(Get-ChildItem (Join-Path $bin "logs") -Filter "ng2_*.log" -ErrorAction SilentlyContinue | Sort-Object Name)
  $p = Start-Process -FilePath $exe -WorkingDirectory $bin -PassThru
  $pid_started = $p.Id
  Write-Host "started ng2.exe pid $pid_started (offload=$($Offload.IsPresent), NG2_TUNE=$tune); running $Seconds s"
  $deadline = (Get-Date).AddSeconds($Seconds)
  while ((Get-Date) -lt $deadline) {
    Start-Sleep -Seconds 5
    if ($p.HasExited) { Write-Host "the game exited on its own (code $($p.ExitCode))"; break }
  }
  if (-not $p.HasExited) {
    Stop-Process -Id $pid_started -Force -ErrorAction SilentlyContinue
    Write-Host "stopped pid $pid_started"
  }
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
  if (Test-Path (Join-Path $backupDlls "rexgpu-xenos.dll")) {
    Copy-Item (Join-Path $backupDlls "rexgpu-xenos.dll") $bin -Force
    Copy-Item (Join-Path $backupDlls "rexruntime.dll") $bin -Force
    Remove-Item $marker -ErrorAction SilentlyContinue
    Write-Host "restored the previous DLL pair"
  }
  if (Test-Path (Join-Path $saveBackup "user")) {
    robocopy (Join-Path $saveBackup "user") (Join-Path $bin "user") /MIR /NFL /NDL /NJH /NJS | Out-Null
    Write-Host "saves restored from $saveBackup (robocopy $LASTEXITCODE)"
  }
  "session=run_native.ps1 game=none until=now note=machine-free (native $Tag done)" | Out-File -Encoding ascii $lockPath
}
