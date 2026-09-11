import ctypes, sys
from ctypes import wintypes

pid = int(sys.argv[1])
BASE = 0x100000000
k = ctypes.windll.kernel32
k.OpenProcess.restype = wintypes.HANDLE
k.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
h = k.OpenProcess(0x0010, False, pid)  # PROCESS_VM_READ
if not h:
    print("OpenProcess failed", ctypes.get_last_error()); sys.exit(1)

def rd(g, n):
    buf = (ctypes.c_ubyte * n)(); got = ctypes.c_size_t(0)
    if not k.ReadProcessMemory(h, ctypes.c_void_p(BASE + g), buf, n, ctypes.byref(got)):
        return None
    return bytes(buf)
def u8(g):    b = rd(g,1); return None if b is None else b[0]
def u16be(g): b = rd(g,2); return None if b is None else (b[0]<<8)|b[1]
def u32be(g): b = rd(g,4); return None if b is None else int.from_bytes(b,"big")

def snap():
    return dict(MODE=u32be(0x84C25070), clr=u32be(0x83BBBFE0), en=u16be(0x84C250C4),
               g1=u8(0x84C23C7A), menu=u8(0x842DC325), LOADSTATE=u16be(0x84299F7A),
               inp=u32be(0x84151694), seq=u32be(0x84C2509C))
if __name__ == "__main__":
    s = snap(); print(" ".join(f"{key}={('None' if v is None else hex(v))}" for key,v in s.items()))
