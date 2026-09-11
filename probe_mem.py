import ctypes, ctypes.wintypes as wt, subprocess
k32 = ctypes.WinDLL("kernel32", use_last_error=True)
k32.OpenProcess.argtypes=[wt.DWORD,wt.BOOL,wt.DWORD]; k32.OpenProcess.restype=wt.HANDLE
k32.ReadProcessMemory.argtypes=[wt.HANDLE,ctypes.c_void_p,ctypes.c_void_p,ctypes.c_size_t,ctypes.POINTER(ctypes.c_size_t)]
k32.VirtualQueryEx.argtypes=[wt.HANDLE,ctypes.c_void_p,ctypes.c_void_p,ctypes.c_size_t]
k32.VirtualQueryEx.restype=ctypes.c_size_t
class MBI(ctypes.Structure):
    _fields_=[("BaseAddress",ctypes.c_ulonglong),("AllocationBase",ctypes.c_ulonglong),
              ("AllocationProtect",wt.DWORD),("__a",wt.DWORD),("RegionSize",ctypes.c_ulonglong),
              ("State",wt.DWORD),("Protect",wt.DWORD),("Type",wt.DWORD),("__b",wt.DWORD)]
out=subprocess.run(["tasklist","/FI","IMAGENAME eq ng2.exe","/FO","CSV"],capture_output=True,text=True).stdout
pid=int([p.strip('"') for p in out.splitlines()[1].split('","')][1])
h=k32.OpenProcess(0x1F0FFF,False,pid)
print("pid",pid,"handle",h,"err",ctypes.get_last_error())
def tryread(a):
    buf=(ctypes.c_char*4)(); got=ctypes.c_size_t()
    ok=k32.ReadProcessMemory(h,ctypes.c_void_p(a),buf,4,ctypes.byref(got))
    return ok, got.value, bytes(buf) if ok else None
for a in (0x3116CAA8C, 0x1116CAA8C, 0x116CAA8C, 0x1B16CAA8C, 0x1D16CAA8C):
    ok,n,b=tryread(a)
    print("  read %014X -> ok=%s n=%d %s" % (a,bool(ok),n,b.hex() if b else ""))
print("--- large committed regions above 4GB ---")
addr=0; n=0
mbi=MBI()
while addr < 0x800000000 and n < 40:
    if not k32.VirtualQueryEx(h,ctypes.c_void_p(addr),ctypes.byref(mbi),ctypes.sizeof(mbi)): break
    if mbi.State==0x1000 and mbi.RegionSize>=0x1000000:
        print("   %014X size %012X prot %03X" % (mbi.BaseAddress,mbi.RegionSize,mbi.Protect)); n+=1
    addr = mbi.BaseAddress + (mbi.RegionSize or 0x1000)
