# Boss-run launcher for the COMPLETE red-mist chapter-transition fix candidate (mode 3).
# Launches ng2.exe with --ng2_chfix3 (arms the fix) + logging, via WMI (the required launch path).
# Then: load your save, play to a chapter boss, kill it, proceed through the mist as normal.
# The fix fires ONLY at the post-boss chapter-complete mist (verified: no false-fire before a boss).
$build = "C:\Users\renoi\ClaudeCode\Ninja Gaiden 2 Xbox360\ng2recomp\out\build\win-amd64-Release"
$exe   = "$build\ng2.exe"
$root  = "C:\Users\renoi\ClaudeCode\Ninja Gaiden 2 Xbox360\ng2recomp\game"
$log   = "C:\Users\renoi\ClaudeCode\Ninja Gaiden 2 Xbox360\ng2recomp\out\bossrun_chfix3.log"
if (Test-Path $log) { Remove-Item $log -Force }
# --ng2_chfix3 arms the fix; drop it (or use --ng2_chfix3=false) for a normal, fix-off run.
$cmd = "`"$exe`" --game_data_root `"$root`" --log_file `"$log`" --log_level info --ng2_chfix3"
$r = Invoke-CimMethod -ClassName Win32_Process -MethodName Create -Arguments @{ CommandLine = $cmd; CurrentDirectory = $build }
"ReturnValue=$($r.ReturnValue)  PID=$($r.ProcessId)"
"log = $log"
