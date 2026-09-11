import ctypes, ctypes.wintypes as wt, subprocess
k32 = ctypes.WinDLL("kernel32", use_last_error=True)
psapi = ctypes.WinDLL("psapi", use_last_error=True)
k32.OpenProcess.argtypes=[wt.DWORD,wt.BOOL,wt.DWORD]; k32.OpenProcess.restype=wt.HANDLE
psapi.EnumProcessModules.argtypes=[wt.HANDLE,ctypes.c_void_p,wt.DWORD,ctypes.POINTER(wt.DWORD)]
psapi.GetModuleFileNameExW.argtypes=[wt.HANDLE,ctypes.c_void_p,ctypes.c_wchar_p,wt.DWORD]
out=subprocess.run(["tasklist","/FI","IMAGENAME eq ng2.exe","/FO","CSV"],capture_output=True,text=True).stdout
pid=int([p.strip('"') for p in out.splitlines()[1].split('","')][1])
h=k32.OpenProcess(0x1F0FFF,False,pid)
mods=(ctypes.c_void_p*512)(); need=wt.DWORD()
psapi.EnumProcessModules(h,ctypes.byref(mods),ctypes.sizeof(mods),ctypes.byref(need))
RIP=0x00007FF7060EBE69
for i in range(need.value//ctypes.sizeof(ctypes.c_void_p)):
    name=ctypes.create_unicode_buffer(260)
    psapi.GetModuleFileNameExW(h,mods[i],name,260)
    base=mods[i] or 0
    n=name.value.split("\\")[-1]
    if n.lower() in ("ng2.exe","rexruntime.dll","rexgpu-xenos.dll"):
        print("%-20s base %016X" % (n, base))
        if n.lower()=="ng2.exe":
            print("   >>> RIP %016X  is RVA 0x%X in ng2.exe" % (RIP, RIP-base))
