"""Read guest memory from a running Xenia process, for comparing correct
behaviour against our recompile.

    python local/diag/xenia_peek.py <xenia_pid> [ADDR[:u8|u16|u32] ...]

With no addresses it just reports the detected guest membase. Xenia reserves a
4 GiB region for guest physical memory; guest VA X lives at membase + X. We find
membase by walking the address space for the big reservation, then verify it by
reading guest 0x82000000 (the code base, always non-zero once a title is up).
"""
import ctypes
import ctypes.wintypes as wt
import struct
import sys

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
PROCESS_QUERY = 0x0400 | 0x0010  # QUERY_INFORMATION | VM_READ
MEM_COMMIT = 0x1000
MEM_RESERVE = 0x2000


class MBI(ctypes.Structure):
    _fields_ = [("BaseAddress", ctypes.c_void_p), ("AllocationBase", ctypes.c_void_p),
                ("AllocationProtect", wt.DWORD), ("__a", wt.DWORD),
                ("RegionSize", ctypes.c_size_t), ("State", wt.DWORD),
                ("Protect", wt.DWORD), ("Type", wt.DWORD), ("__b", wt.DWORD)]


def read(h, host, n):
    buf = (ctypes.c_ubyte * n)()
    got = ctypes.c_size_t()
    if not k32.ReadProcessMemory(h, ctypes.c_void_p(host), buf, n, ctypes.byref(got)) or got.value != n:
        return None
    return bytes(buf)


def find_membase(h):
    # Candidates first (fast path): Xenia commonly maps at 0x100000000.
    for cand in (0x100000000, 0x200000000, 0x300000000):
        data = read(h, cand + 0x82000000, 4)
        if data and data != b"\x00\x00\x00\x00":
            return cand
    # Fallback: walk for a >= 4 GiB reservation, test each base.
    addr = 0
    mbi = MBI()
    while addr < 0x800000000000:
        if not k32.VirtualQueryEx(h, ctypes.c_void_p(addr), ctypes.byref(mbi), ctypes.sizeof(mbi)):
            break
        base = mbi.BaseAddress or 0
        size = mbi.RegionSize or 0x1000
        if size >= 0x100000000 and (mbi.State & (MEM_RESERVE | MEM_COMMIT)):
            data = read(h, base + 0x82000000, 4)
            if data and data != b"\x00\x00\x00\x00":
                return base
        addr = base + size
    return None


def main():
    pid = int(sys.argv[1])
    h = k32.OpenProcess(PROCESS_QUERY, False, pid)
    if not h:
        print("OpenProcess failed:", ctypes.get_last_error())
        return 1
    base = find_membase(h)
    if base is None:
        print("could not locate Xenia guest membase")
        return 1
    print("guest membase = 0x%X" % base)
    for spec in sys.argv[2:]:
        a, _, kind = spec.partition(":")
        va = int(a, 16)
        kind = kind or "u32"
        w = {"u8": 1, "u16": 2, "u32": 4}[kind]
        data = read(h, base + va, w)
        if data is None:
            print("0x%08X %s = <unreadable>" % (va, kind))
            continue
        v = int.from_bytes(data, "big")
        print("0x%08X %s = %d (0x%0*X)" % (va, kind, v, w * 2, v))
    k32.CloseHandle(h)
    return 0


if __name__ == "__main__":
    sys.exit(main())
