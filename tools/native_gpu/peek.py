"""Hex-dump guest memory of a running ReXGlue game (big-endian words).
usage: peek.py <process name> <guest_hex> [len=256]
"""
import ctypes as C, struct, subprocess, sys

proc, a = sys.argv[1], int(sys.argv[2], 16)
n = int(sys.argv[3], 0) if len(sys.argv) > 3 else 256
k = C.WinDLL("kernel32", use_last_error=True)
k.OpenProcess.restype = C.c_void_p
k.ReadProcessMemory.argtypes = [C.c_void_p, C.c_void_p, C.c_void_p, C.c_size_t, C.POINTER(C.c_size_t)]
pid = int(subprocess.run(["powershell", "-NoProfile", "-Command", f"(Get-Process {proc})[0].Id"],
                         capture_output=True, text=True).stdout.strip())
hp = k.OpenProcess(0x0410, False, pid)
buf = (C.c_char * n)(); got = C.c_size_t(0)
if not k.ReadProcessMemory(hp, C.c_void_p(0x100000000 + a), buf, n, C.byref(got)):
    sys.exit(f"read failed at {a:08X}: {C.get_last_error()}")
data = buf.raw[:got.value]
for off in range(0, len(data) - 3, 16):
    ws = struct.unpack(">" + "I" * (min(16, len(data) - off) // 4), data[off:off + (min(16, len(data) - off) // 4) * 4])
    print(f"{a + off:08X}  " + " ".join(f"{w:08X}" for w in ws))
