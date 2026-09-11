# Is the game still feeding audio to Windows?
#
# This is the question that splits the fault in two, and neither the game's log
# nor SDL can answer it: the log is silent because nothing errors, and SDL will
# happily accept samples for a stream nobody hears. Windows knows, because it
# meters every session.
#
#   session present, peak > 0   -> audio IS reaching Windows; the fault is the
#                                  endpoint or routing (wrong device, S/PDIF
#                                  asleep, receiver renegotiated).
#   session present, peak == 0  -> the game has stopped producing sound; the
#                                  fault is upstream, in the guest or the APU.
#   no session at all           -> the stream was closed or never reopened.

$ErrorActionPreference = 'Stop'

Add-Type -Language CSharp @'
using System;
using System.Runtime.InteropServices;

public static class Aud {
  [ComImport, Guid("BCDE0395-E52F-467C-8E3D-C4579291692E")]
  class MMDeviceEnumerator { }

  [ComImport, Guid("A95664D2-9614-4F35-A746-DE8DB63617E6"),
   InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
  interface IMMDeviceEnumerator {
    int NotImpl1();
    int GetDefaultAudioEndpoint(int dataFlow, int role, out IMMDevice ppEndpoint);
  }

  [ComImport, Guid("D666063F-1587-4E43-81F1-B948E807363F"),
   InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
  interface IMMDevice {
    int Activate(ref Guid iid, int dwClsCtx, IntPtr pActivationParams,
                 [MarshalAs(UnmanagedType.IUnknown)] out object ppInterface);
  }

  [ComImport, Guid("77AA99A0-1BD6-484F-8BC7-2C654C9A9B6F"),
   InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
  interface IAudioSessionManager2 {
    int NotImpl1();
    int NotImpl2();
    int GetSessionEnumerator(out IAudioSessionEnumerator SessionEnum);
  }

  [ComImport, Guid("E2F5BB11-0570-40CA-ACDD-3AA01277DEE8"),
   InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
  interface IAudioSessionEnumerator {
    int GetCount(out int SessionCount);
    int GetSession(int SessionCount, out IAudioSessionControl Session);
  }

  [ComImport, Guid("F4B1A599-7266-4319-A8CA-E70ACB11E8CD"),
   InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
  interface IAudioSessionControl {
    int GetState(out int pRetVal);
  }

  [ComImport, Guid("BFB7FF88-7239-4FC9-8FA2-07C950BE9C6D"),
   InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
  interface IAudioSessionControl2 {
    int GetState(out int pRetVal);
    int NotImpl1();  int NotImpl2();  int NotImpl3();  int NotImpl4();
    int NotImpl5();  int NotImpl6();  int NotImpl7();  int NotImpl8();
    int GetSessionIdentifier(out IntPtr pRetVal);
    int GetSessionInstanceIdentifier(out IntPtr pRetVal);
    int GetProcessId(out uint pRetVal);
  }

  [ComImport, Guid("C02216F6-8C67-4B5B-9D00-D008E73E0064"),
   InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
  interface IAudioMeterInformation {
    int GetPeakValue(out float pfPeak);
  }

  public static string Report(uint wantPid) {
    var en = (IMMDeviceEnumerator)(new MMDeviceEnumerator());
    IMMDevice dev;
    // 0 = eRender, 0 = eConsole
    en.GetDefaultAudioEndpoint(0, 0, out dev);

    var mgrIid = typeof(IAudioSessionManager2).GUID;
    object mgrObj;
    dev.Activate(ref mgrIid, 1, IntPtr.Zero, out mgrObj);
    var mgr = (IAudioSessionManager2)mgrObj;

    IAudioSessionEnumerator sessions;
    mgr.GetSessionEnumerator(out sessions);
    int count;
    sessions.GetCount(out count);

    string outp = "default render device sessions: " + count + "\n";
    bool found = false;
    for (int i = 0; i < count; i++) {
      IAudioSessionControl ctl;
      sessions.GetSession(i, out ctl);
      var c2 = (IAudioSessionControl2)ctl;
      uint pid;
      if (c2.GetProcessId(out pid) != 0) continue;
      if (pid != wantPid) continue;
      found = true;
      int state; c2.GetState(out state);
      var meter = (IAudioMeterInformation)ctl;
      float peak = -1f;
      meter.GetPeakValue(out peak);
      string st = state == 0 ? "Inactive" : (state == 1 ? "ACTIVE" : "Expired");
      outp += "  pid " + pid + "  state=" + st + "  peak=" + peak.ToString("F6") + "\n";
    }
    if (!found) outp += "  NO SESSION for pid " + wantPid + "\n";
    return outp;
  }
}
'@

$proc = Get-Process ng2 -ErrorAction SilentlyContinue
if (-not $proc) { "ng2 is not running"; exit }

# Sample repeatedly: a peak is an instant, and one zero reading proves nothing.
$maxPeak = 0.0
for ($i = 0; $i -lt 25; $i++) {
  $r = [Aud]::Report([uint32]$proc.Id)
  if ($i -eq 0) { $r.Split("`n")[0] }
  foreach ($line in $r.Split("`n")) {
    if ($line -match 'peak=([0-9.]+)') {
      $p = [double]$Matches[1]
      if ($p -gt $maxPeak) { $maxPeak = $p }
    }
    if ($line -match 'NO SESSION') { $line; break }
  }
  Start-Sleep -Milliseconds 120
}
($r.Split("`n") | Where-Object { $_ -match 'pid|NO SESSION' }) -join "`n"
"peak over 3s of sampling: $($maxPeak.ToString('F6'))"
if ($maxPeak -gt 0.0001) {
  "VERDICT: audio IS reaching Windows - the fault is the endpoint/routing, not the game."
} else {
  "VERDICT: nothing is being produced - the fault is upstream of Windows (guest or APU)."
}
