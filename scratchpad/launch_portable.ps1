# Launch the portable folder's ng2.exe through WMI with a FOREIGN working
# directory (C:\), which is the strict test that every path in its settings
# resolves against the executable's folder rather than the working directory.
param([string]$Dest = "D:\Ninja Gaiden 2 Portable")
$cmd = "`"$Dest\ng2.exe`""
$r = Invoke-CimMethod -ClassName Win32_Process -MethodName Create -Arguments @{ CommandLine = $cmd; CurrentDirectory = "C:\" }
"ReturnValue=$($r.ReturnValue)  PID=$($r.ProcessId)  (working directory C:\)"
