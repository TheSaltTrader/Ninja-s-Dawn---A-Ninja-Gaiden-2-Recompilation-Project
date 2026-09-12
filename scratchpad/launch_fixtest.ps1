# Verification launcher for the chapter award fix (src/ng2_chapter_fix.cpp): NO --ng2_chfix3,
# so the press-driven remedy and the hold hatch stay off and only the new fix is in play.
# Launches via WMI (the required launch path) with logging to out/fixtest.log.
$build = "C:\Users\renoi\ClaudeCode\Ninja Gaiden 2 Xbox360\ng2recomp\out\build\win-amd64-Release"
$exe   = "$build\ng2.exe"
$root  = "C:\Users\renoi\ClaudeCode\Ninja Gaiden 2 Xbox360\ng2recomp\game"
$log   = "C:\Users\renoi\ClaudeCode\Ninja Gaiden 2 Xbox360\ng2recomp\out\fixtest.log"
# Keep the previous session's log instead of deleting it: a relaunch right after
# a crash used to erase the only evidence of the crash.
if (Test-Path $log) {
  $stamp = (Get-Item $log).LastWriteTime.ToString("yyyyMMdd_HHmmss")
  Move-Item $log ($log -replace '\.log$', "_$stamp.log") -Force
}
$cmd = "`"$exe`" --game_data_root `"$root`" --log_file `"$log`" --log_level info"
$r = Invoke-CimMethod -ClassName Win32_Process -MethodName Create -Arguments @{ CommandLine = $cmd; CurrentDirectory = $build }
"ReturnValue=$($r.ReturnValue)  PID=$($r.ProcessId)"
"log = $log"
