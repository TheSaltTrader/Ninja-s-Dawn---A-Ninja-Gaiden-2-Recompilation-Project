# Watch the running game and AUTO-CAPTURE the instant a rare event fires.
#
# The music death and the attract freeze are intermittent and can happen while
# no one is looking; if the process is then closed the evidence is gone. This
# watcher takes the capture itself, the moment it sees the trigger, so a single
# rare repro is never wasted:
#
#   - audio peak drops to 0 (after having produced sound)  -> music death
#   - a new "RINGBUFFER: Failed" line appears              -> attract ring fault
#   - the log goes silent while the process is alive       -> freeze
#
# On any trigger it runs capture_hang.sh (non-invasive cdb; cannot kill the
# target) and writes the dump next to this script, then keeps watching so a
# freeze that follows a ring fault is also caught.
param(
  [int]$TargetPid,
  [string]$Root = "C:\Users\renoi\ClaudeCode\Ninja Gaiden 2 Xbox360\ng2recomp"
)
$ErrorActionPreference = 'SilentlyContinue'
$LogDir  = "$Root\out\build\win-amd64-Release\logs"
$OutDir  = "$Root\local\diag\captures"
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$bash = "C:\Program Files\Git\bin\bash.exe"
if (-not (Test-Path $bash)) { $bash = "bash" }

Add-Type -TypeDefinition @'
using System;using System.Runtime.InteropServices;
[Guid("BCDE0395-E52F-467C-8E3D-C4579291692E")]class MMDeviceEnumerator{}
[Guid("A95664D2-9614-4F35-A746-DE8DB63617E6"),InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
interface IMMDeviceEnumerator{int f();int GetDefaultAudioEndpoint(int d,int r,out IMMDevice e);}
[Guid("D666063F-1587-4E43-81F1-B948E807363F"),InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
interface IMMDevice{int Activate(ref Guid id,int ctx,IntPtr p,[MarshalAs(UnmanagedType.IUnknown)]out object o);}
[Guid("77AA99A0-1BD6-484F-8BC7-2C654C9A9B6F"),InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
interface IAudioSessionManager2{int f1();int f2();int f3();int f4();int GetSessionEnumerator(out IAudioSessionEnumerator e);}
[Guid("E2F5BB11-0570-40CA-ACDD-3AA01277DEE8"),InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
interface IAudioSessionEnumerator{int GetCount(out int c);int GetSession(int i,out IAudioSessionControl2 s);}
[Guid("BFB7FF88-7239-4FC9-8FA2-07C950BE9C6D"),InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
interface IAudioSessionControl2{int f1();int f2();int f3();int f4();int f5();int f6();int f7();int f8();int f9();int f10();int f11();int GetProcessId(out uint pid);}
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

function Capture([string]$why){
  $stamp = Get-Date -Format "HHmmss"
  $out = "$OutDir\cap_${why}_$stamp.txt"
  "  >>> AUTO-CAPTURE ($why) -> $out"
  & $bash "$Root\local\diag\capture_hang.sh" $TargetPid *> $null
  # capture_hang writes hang_<HHMMSS>.txt into the scratchpad; also take our own
  # copy here so it survives. Re-run with explicit redirect for durability.
  & $bash -c "cd '$($Root -replace '\\','/')/local/diag'; bash capture_hang.sh $TargetPid" > $out 2>&1
  "  >>> capture written ($((Get-Item $out).Length) bytes)"
}

$log = Get-ChildItem "$LogDir\ng2_*.log" | Sort-Object LastWriteTime -Desc | Select-Object -First 1
"watching pid $TargetPid, log $($log.Name); auto-capture -> $OutDir"
$peakEver=0.0; $silent=0; $seenRing=0; $seenWait=0; $capturedAudio=$false; $capturedRing=$false; $quiet=$false

while($true){
  Start-Sleep -Seconds 15
  $p = Get-Process -Id $TargetPid -ErrorAction SilentlyContinue
  if(-not $p){ "PROCESS GONE: pid $TargetPid exited"; break }

  # audio
  $pk=[Meter]::PeakFor([uint32]$TargetPid)
  if($pk -gt $peakEver){ $peakEver=$pk }
  if($pk -eq 0.0 -and $peakEver -gt 0.01){
    $silent++
    if($silent -eq 2 -and -not $capturedAudio){
      "AUDIO DEAD: peak 0 (peaked $([math]::Round($peakEver,3))) - music bug"
      Capture "audio"; $capturedAudio=$true
    }
  } elseif($pk -gt 0.0){ $silent=0; $capturedAudio=$false }

  # ring
  $log.Refresh()
  $txt = Get-Content $log.FullName -ErrorAction SilentlyContinue
  $ring = ($txt | Select-String 'RINGBUFFER: Failed').Count
  if($ring -gt $seenRing){
    "RING FAULT: $($ring-$seenRing) new (total $ring)"
    $dump = $txt | Select-String 'ring: base|write pointers|  wptr |primary passes|read -> write|pass '
    if($dump){ $dump | Select-Object -Last 12 | ForEach-Object { "   $_" } }
    if(-not $capturedRing){ Capture "ring"; $capturedRing=$true }
    $seenRing=$ring
  }

  # stuck waits worth surfacing (deadlock candidates only, not the benign two)
  $waits = $txt | Select-String 'has been waiting' | Where-Object { $_ -notmatch '40004BC4|F80000CC' }
  if($waits.Count -gt $seenWait){
    $waits | Select-Object -Skip $seenWait | ForEach-Object { "WATCHDOG(new): " + ($_ -replace '.*\[ng2\] ','') }
    $seenWait=$waits.Count
  }

  # freeze
  $gap=[int]((Get-Date)-$log.LastWriteTime).TotalSeconds
  if($gap -ge 45){
    if(-not $quiet){ "LOG QUIET ${gap}s while alive - possible freeze"; Capture "freeze"; $quiet=$true }
  } else { $quiet=$false }
}
