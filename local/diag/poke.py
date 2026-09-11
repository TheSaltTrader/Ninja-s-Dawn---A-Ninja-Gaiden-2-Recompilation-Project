"""Write guest words into a live game (WriteProcessMemory through the 1:1 map).

    python local/diag/poke.py <pid> ADDR=VALUE[:u8|u16|u32] ...

ADDR and VALUE are hex (0x optional). Default width u32 big-endian - the guest
is big-endian, so a u32 poke of 6 writes 00 00 00 06 at ADDR. Reads back and
prints old -> new for every poke so a no-op or a bad address is obvious.
"""
import ctypes
import ctypes.wintypes as wt
import struct
import sys

GUEST_BASE = 0x1_0000_0000
k32 = ctypes.WinDLL("kernel32", use_last_error=True)
PROCESS_ALL = 0x1F0FFF


def main():
    pid = int(sys.argv[1])
    h = k32.OpenProcess(PROCESS_ALL, False, pid)
    if not h:
        print("OpenProcess failed:", ctypes.get_last_error())
        return 1
    try:
        for spec in sys.argv[2:]:
            lhs, _, rhs = spec.partition("=")
            val_s, _, kind = rhs.partition(":")
            kind = kind or "u32"
            va = int(lhs, 16)
            val = int(val_s, 16)
            host = GUEST_BASE + va
            width = {"u8": 1, "u16": 2, "u32": 4}[kind]
            fmt = {"u8": ">B", "u16": ">H", "u32": ">I"}[kind]
            # read old
            old = (ctypes.c_ubyte * width)()
            got = ctypes.c_size_t()
            k32.ReadProcessMemory(h, ctypes.c_void_p(host), old, width, ctypes.byref(got))
            old_v = int.from_bytes(bytes(old), "big")
            data = struct.pack(fmt, val)
            buf = (ctypes.c_ubyte * width).from_buffer_copy(data)
            wrote = ctypes.c_size_t()
            ok = k32.WriteProcessMemory(h, ctypes.c_void_p(host), buf, width, ctypes.byref(wrote))
            # read back
            nb = (ctypes.c_ubyte * width)()
            k32.ReadProcessMemory(h, ctypes.c_void_p(host), nb, width, ctypes.byref(got))
            new_v = int.from_bytes(bytes(nb), "big")
            print("0x%08X %s: 0x%0*X -> 0x%0*X  (write %s, %d bytes)" % (
                va, kind, width * 2, old_v, width * 2, new_v,
                "ok" if ok else "FAILED %d" % ctypes.get_last_error(), wrote.value))
    finally:
        k32.CloseHandle(h)
    return 0


if __name__ == "__main__":
    sys.exit(main())
