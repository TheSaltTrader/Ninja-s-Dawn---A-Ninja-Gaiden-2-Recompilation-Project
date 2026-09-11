# Write bytes into the running recomp's guest memory (host = 0x100000000 + guest, big-endian).
# Usage: python poke.py <PID> <guest_hex> <byte_hex> [<byte_hex> ...]
# Example: python poke.py 66832 84B5030A 01
import ctypes, sys
from ctypes import wintypes

pid = int(sys.argv[1]); guest = int(sys.argv[2], 16); data = bytes(int(b, 16) for b in sys.argv[3:])
BASE = 0x100000000
k = ctypes.windll.kernel32
k.OpenProcess.restype = wintypes.HANDLE
k.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
k.WriteProcessMemory.argtypes = [wintypes.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
k.ReadProcessMemory.argtypes = [wintypes.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
h = k.OpenProcess(0x0038 | 0x0010, False, pid)  # VM_OPERATION | VM_WRITE | VM_READ
if not h:
    print("OpenProcess failed", ctypes.get_last_error()); sys.exit(1)
buf = (ctypes.c_ubyte * len(data))(); got = ctypes.c_size_t(0)
k.ReadProcessMemory(h, ctypes.c_void_p(BASE + guest), buf, len(data), ctypes.byref(got))
print(f"before: {bytes(buf).hex()}")
wbuf = (ctypes.c_ubyte * len(data))(*data); wrote = ctypes.c_size_t(0)
ok = k.WriteProcessMemory(h, ctypes.c_void_p(BASE + guest), wbuf, len(data), ctypes.byref(wrote))
k.ReadProcessMemory(h, ctypes.c_void_p(BASE + guest), buf, len(data), ctypes.byref(got))
print(f"write ok={bool(ok)} n={wrote.value}  after: {bytes(buf).hex()}")
