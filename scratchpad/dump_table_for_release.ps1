# Map crash-dump RVAs of an OLD build to guest functions: run a throwaway copy of
# that release with the quit seam, make it dump its guest->host function table
# (tools/dump_table_live.py -> C:/ng2dump/functable.txt), and record the module
# base so RVAs from the dumps can be matched.
#   powershell -File scratchpad\dump_table_for_release.ps1 -Release v1.0.8
param([string]$Release = "v1.0.8")
$base = "C:\Users\renoi\ClaudeCode\Ninja Gaiden 2 Xbox360"
$src  = "$base\Releases\$Release"
$tmp  = "C:\Users\renoi\AppData\Local\Temp\claude\C--users-renoi-claudecode\a1d8592e-3eb7-4b65-abd2-56136e137a92\scratchpad\rel_$Release"
robocopy $src $tmp /E /NFL /NDL /NJH /NJS /NP | Out-Null
New-Item -ItemType Directory -Force "C:\ng2dump" | Out-Null
$root = "$base\ng2recomp\game"
$log  = "$tmp\table.log"
$cmd  = "`"$tmp\ng2.exe`" --game_data_root `"$root`" --log_file `"$log`" --log_level info"
$envList = @(Get-ChildItem env: | ForEach-Object { "$($_.Name)=$($_.Value)" }) + @("NG2_QUIT_AFTER=70")
$su = New-CimInstance -ClassName Win32_ProcessStartup -ClientOnly -Property @{ EnvironmentVariables = [string[]]$envList }
$r = Invoke-CimMethod -ClassName Win32_Process -MethodName Create -Arguments @{ CommandLine = $cmd; CurrentDirectory = $tmp; ProcessStartupInformation = $su }
"launched $Release copy as PID $($r.ProcessId)"
Start-Sleep -Seconds 25
$p = Get-Process -Id $r.ProcessId -ErrorAction SilentlyContinue
if (-not $p) { "process gone before the dump"; exit 1 }
$m = $p.Modules | Where-Object { $_.ModuleName -ieq "ng2.exe" } | Select-Object -First 1
"ng2.exe base 0x{0:X}" -f $m.BaseAddress.ToInt64()
& python "$base\ng2recomp\tools\dump_table_live.py"
Start-Sleep -Seconds 2
if (Test-Path "C:\ng2dump\functable.txt") { "table lines: " + (Get-Content "C:\ng2dump\functable.txt" | Measure-Object -Line).Lines; Get-Content "C:\ng2dump\functable.txt" -TotalCount 3 }
Stop-Process -Id $r.ProcessId -Force -ErrorAction SilentlyContinue
"closed PID $($r.ProcessId)"
