# Morning test: launch the build with resolve-at-load ENABLED (NG2_RESOLVE_AT_LOAD=1).
# This is the fix for chapter 10's missing upscales: pack textures are matched by
# the bytes in memory at LOAD, not at creation. Default is OFF; this run turns it on.
# Play chapter 10 and watch whether the previously-bare surfaces upscale.
$build = "C:\Users\renoi\ClaudeCode\Ninja Gaiden 2 Xbox360\ng2recomp\out\build\win-amd64-Release"
$root  = "C:\Users\renoi\ClaudeCode\Ninja Gaiden 2 Xbox360\ng2recomp\game"
$log   = "C:\Users\renoi\ClaudeCode\Ninja Gaiden 2 Xbox360\ng2recomp\out\resolvetest.log"
if (Test-Path $log) { Move-Item $log ($log -replace '\.log$', ("_" + (Get-Item $log).LastWriteTime.ToString("HHmmss") + ".log")) -Force }
$envList = @(Get-ChildItem env: | ForEach-Object { "$($_.Name)=$($_.Value)" }) + @("NG2_RESOLVE_AT_LOAD=1")
$su = New-CimInstance -ClassName Win32_ProcessStartup -ClientOnly -Property @{ EnvironmentVariables = [string[]]$envList }
$cmd = "`"$build\ng2.exe`" --game_data_root `"$root`" --log_file `"$log`" --log_level info"
$r = Invoke-CimMethod -ClassName Win32_Process -MethodName Create -Arguments @{ CommandLine = $cmd; CurrentDirectory = $build; ProcessStartupInformation = $su }
"launched PID $($r.ProcessId) with resolve-at-load ON"
"log = $log"
"After playing chapter 10, these tell the story:"
'  grep "resolve-at-load: no pack file" the log  -> should be rare/none (content that was never dumped)'
'  grep "none matches" the log                    -> should be GONE for the textures that now upscale'
