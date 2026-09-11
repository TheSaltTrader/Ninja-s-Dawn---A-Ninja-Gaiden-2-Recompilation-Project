# Measure the Escape exit. Launches the build through WMI (the required launch
# path) with NG2_QUIT_AFTER=<s>, which fires Escape's own code from a timer once
# the title screen is up, then times the process from "Escape: quitting" to gone.
#   powershell -File scratchpad\quit_test.ps1 [seconds-before-quit]
param([int]$After = 60)
$build = "C:\Users\renoi\ClaudeCode\Ninja Gaiden 2 Xbox360\ng2recomp\out\build\win-amd64-Release"
$exe   = "$build\ng2.exe"
$root  = "C:\Users\renoi\ClaudeCode\Ninja Gaiden 2 Xbox360\ng2recomp\game"
$log   = "C:\Users\renoi\ClaudeCode\Ninja Gaiden 2 Xbox360\ng2recomp\out\quittest.log"
if (Test-Path $log) { Remove-Item $log -Force }
$cmd = "`"$exe`" --game_data_root `"$root`" --log_file `"$log`" --log_level info"
# Win32_ProcessStartup.EnvironmentVariables REPLACES the environment (a seam-only
# array left the game without SystemRoot or an audio device and it died at boot),
# so hand over the whole current environment plus the seam.
$envList = @(Get-ChildItem env: | ForEach-Object { "$($_.Name)=$($_.Value)" }) + @("NG2_QUIT_AFTER=$After")
$su = New-CimInstance -ClassName Win32_ProcessStartup -ClientOnly -Property @{ EnvironmentVariables = [string[]]$envList }
$r = Invoke-CimMethod -ClassName Win32_Process -MethodName Create -Arguments @{ CommandLine = $cmd; CurrentDirectory = $build; ProcessStartupInformation = $su }
$pid_ = $r.ProcessId
"launched PID $pid_ with NG2_QUIT_AFTER=$After"
# Wait for the quit line, then time the exit at 20 ms resolution.
$deadline = (Get-Date).AddSeconds($After + 40)
$quitAt = $null
while ((Get-Date) -lt $deadline) {
  $line = $null
  if (Test-Path $log) { $line = Select-String -Path $log -Pattern "Escape: quitting" | Select-Object -First 1 }
  if ($line) { $quitAt = Get-Date; break }
  if (-not (Get-Process -Id $pid_ -ErrorAction SilentlyContinue)) { "process exited before the seam fired"; break }
  Start-Sleep -Milliseconds 100
}
if ($quitAt) {
  $sw = [System.Diagnostics.Stopwatch]::StartNew()
  while ((Get-Process -Id $pid_ -ErrorAction SilentlyContinue) -and $sw.Elapsed.TotalSeconds -lt 15) { Start-Sleep -Milliseconds 20 }
  $sw.Stop()
  if (Get-Process -Id $pid_ -ErrorAction SilentlyContinue) { "STILL RUNNING 15 s after the quit line"; Stop-Process -Id $pid_ -Force }
  else { "process gone {0:N2} s after 'Escape: quitting' was seen (polling adds up to 0.1 s)" -f $sw.Elapsed.TotalSeconds }
}
"--- log tail (quit lines):"
Select-String -Path $log -Pattern "Quit seam|Escape: quitting|Shutdown|Title terminated|did not finish" | ForEach-Object { $_.Line.Substring(0, [Math]::Min(140, $_.Line.Length)) }
