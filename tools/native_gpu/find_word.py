"""Find every guest address holding a given big-endian 32-bit value in a
running ReXGlue game (e.g. the global that holds the Direct3D device pointer).
usage: find_word.py <process name> <value_hex> [scan_lo_hex scan_hi_hex]
"""
import ctypes as C, subprocess, sys
import numpy as np

proc, val = sys.argv[1], int(sys.argv[2], 16)
scan_lo = int(sys.argv[3], 16) if len(sys.argv) > 3 else 0x00010000
scan_hi = int(sys.argv[4], 16) if len(sys.argv) > 4 else 0xC0000000
k = C.WinDLL("kernel32", use_last_error=True)
k.OpenProcess.restype = C.c_void_p
k.ReadProcessMemory.argtypes = [C.c_void_p, C.c_void_p, C.c_void_p, C.c_size_t, C.POINTER(C.c_size_t)]
pid = int(subprocess.run(["powershell", "-NoProfile", "-Command", f"(Get-Process {proc})[0].Id"],
                         capture_output=True, text=True).stdout.strip())
hp = k.OpenProcess(0x0410, False, pid)
CH = 1 << 20
buf = (C.c_char * CH)()
addr = scan_lo
while addr < scan_hi:
    got = C.c_size_t(0)
    if k.ReadProcessMemory(hp, C.c_void_p(0x100000000 + addr), buf, CH, C.byref(got)) and got.value >= 4:
        words = np.frombuffer(buf.raw[: (got.value // 4) * 4], dtype=">u4")
        for i in np.flatnonzero(words == val):
            print(f"0x{addr + int(i) * 4:08X}")
    addr += CH
