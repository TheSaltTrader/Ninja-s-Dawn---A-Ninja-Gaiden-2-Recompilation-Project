# Long watch for the music death, with the new wait instrumentation live.
#
# Emits a line ONLY on something worth acting on. Silence here means the run is
# healthy - but silence is also what a freeze looks like, so a stalled log and a
# dead process are themselves events. The audio check is the per-process peak
# meter rather than anything the game reports, because the failure being hunted
# is precisely the one where the game believes it is still playing.
param(
  [int]$TargetPid,
  [string]$LogDir = "C:\Users\renoi\ClaudeCode\Ninja Gaiden 2 Xbox360\ng2recomp\out\build\win-amd64-Release\logs"
)
$ErrorActionPreference = 'SilentlyContinue'

Add-Type -TypeDefinition @'
using System;using System.Runtime.InteropServices;
[Guid("BCDE0395-E52F-467C-8E3D-C4579291692E")]class MMDeviceEnumerator{}
[Guid("A95664D2-9614-4F35-A746-DE8DB63617E6"),InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
interface IMMDeviceEnumerator{int f();int GetDefaultAudioEndpoint(int d,int r,out IMMDevice e);}
[Guid("D666063F-1587-4E43-81F1-B948E807363F"),InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
interface IMMDevice{int Activate(ref Guid id,int ctx,IntPtr p,[MarshalAs(UnmanagedType.IUnknown)]out object o);}
[Guid("77AA99A0-1BD6-484F-8BC7-2C654C9A9B6F"),InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
interface IAudioSessionManager2{int f1();int f2();int f3();int f4();
  int GetSessionEnumerator(out IAudioSessionEnumerator e);}
[Guid("E2F5BB11-0570-40CA-ACDD-3AA01277DEE8"),InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
interface IAudioSessionEnumerator{int GetCount(out int c);int GetSession(int i,out IAudioSessionControl2 s);}
[Guid("BFB7FF88-7239-4FC9-8FA2-07C950BE9C6D"),InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
interface IAudioSessionControl2{int f1();int f2();int f3();int f4();int f5();int f6();int f7();int f8();
  int f9();int f10();int f11();int GetProcessId(out uint pid);}
[Guid("C02216F6-8C67-4B5B-9D00-D008E73E0064"),InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
interface IAudioMeterInformation{int GetPeakValue(out float p);}
public static class Meter{
  public static float PeakFor(uint target){
    var en=(IMMDeviceEnumerator)(new MMDeviceEnumerator() as object);
    IMMDevice dev; if(en.GetDefaultAudioEndpoint(0,0,out dev)!=0) return -1f;
    var gm=new Guid("77AA99A0-1BD6-484F-8BC7-2C654C9A9B6F"); object o;
    if(dev.Activate(ref gm,1,IntPtr.Zero,out o)!=0) return -1f;
    IAudioSessionEnumerator se; if(((IAudioSessionManager2)o).GetSessionEnumerator(out se)!=0) return -1f;
    int n; se.GetCount(out n);
    for(int i=0;i<n;i++){ IAudioSessionControl2 sc; if(se.GetSession(i,out sc)!=0) continue;
      uint pid; if(sc.GetProcessId(out pid)!=0) continue; if(pid!=target) continue;
      var mi=sc as IAudioMeterInformation; if(mi==null) continue;
      float pk; if(mi.GetPeakValue(out pk)==0) return pk; }
    return -2f; }
}
'@ -ReferencedAssemblies System.Runtime.InteropServices 2>$null

$log = Get-ChildItem "$LogDir\ng2_*.log" | Sort-Object LastWriteTime -Desc | Select-Object -First 1
"watching pid $TargetPid, log $($log.Name)"
$seenRing = 0; $seenWait = 0; $silent = 0; $quietReported = $false; $peakEver = 0.0

while ($true) {
  Start-Sleep -Seconds 20
  $proc = Get-Process -Id $TargetPid -ErrorAction SilentlyContinue
  if (-not $proc) { "PROCESS GONE: ng2 pid $TargetPid exited"; break }

  $log.Refresh()
  $gap = [int]((Get-Date) - $log.LastWriteTime).TotalSeconds
  if ($gap -ge 60) {
    if (-not $quietReported) { "LOG QUIET: no lines for ${gap}s while pid $TargetPid is alive - likely frozen"; $quietReported = $true }
  } else { $quietReported = $false }

  $txt = Get-Content $log.FullName -ErrorAction SilentlyContinue
  $ring = ($txt | Select-String 'RINGBUFFER: Failed').Count
  if ($ring -gt $seenRing) { "RING: $($ring - $seenRing) new ringbuffer failures (total $ring)"; $seenRing = $ring }
  $waits = $txt | Select-String 'has been waiting'
  if ($waits.Count -gt $seenWait) {
    $waits | Select-Object -Skip $seenWait | ForEach-Object { "WATCHDOG: " + ($_ -replace '.*\[ng2\] ','') }
    $seenWait = $waits.Count
  }

  $pk = [Meter]::PeakFor([uint32]$TargetPid)
  if ($pk -gt $peakEver) { $peakEver = $pk }
  if ($pk -eq 0.0 -and $peakEver -gt 0.001) {
    $silent++
    if ($silent -eq 3) { "AUDIO DEAD: peak 0 for 3 consecutive checks (peaked at $([math]::Round($peakEver,4)) earlier) - music bug reproduced" }
  } elseif ($pk -gt 0.0) { $silent = 0 }
}
