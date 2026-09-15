"""Find the Direct3D device's function-pointer tables in a RUNNING game's guest
memory: runs of big-endian 32-bit code addresses stored consecutively
(stride 4) or with a stride of 12 (the XDK's sampler table). "Code address"
means either inside [lib_lo, lib_hi] or, with --funcs <game_dir>, exactly the
address of a recompiled function (DEFINE_REX_FUNC in generated/default) - the
precise test, which excludes vtables (they point at data). Guest -> host
mapping is flat (host = guest + 0x100000000).
usage: scan_tables.py <process name> <lib_lo_hex> <lib_hi_hex> [scan_lo_hex scan_hi_hex]
                      [--min N] [--funcs <game_dir>] [--within lo_hex hi_hex]
--within: keep only tables whose targets all fall in that range (after the
function-set test), e.g. the D3D library.
"""
import ctypes as C, glob, os, re, subprocess, sys
import numpy as np


def opt(name, n=1, default=None):
    if name not in sys.argv:
        return default
    i = sys.argv.index(name)
    vals = sys.argv[i + 1:i + 1 + n]
    del sys.argv[i:i + 1 + n]
    return vals if n > 1 else vals[0]


minlen = int(opt("--min", default="12"))
funcs_dir = opt("--funcs")
within = opt("--within", 2)
args = sys.argv[1:]
proc = args[0]; lo = int(args[1], 16); hi = int(args[2], 16)
scan_lo = int(args[3], 16) if len(args) > 3 else 0x00010000
scan_hi = int(args[4], 16) if len(args) > 4 else 0xC0000000

func_set = None
if funcs_dir:
    cache = os.path.join(funcs_dir, "generated", "func_addrs.npy")
    if os.path.exists(cache):
        func_set = np.load(cache)
    else:
        addrs = set()
        rx = re.compile(r"^DEFINE_REX_FUNC\(sub_([0-9A-Fa-f]{8})\)", re.M)
        for path in glob.glob(os.path.join(funcs_dir, "generated", "default", "*_recomp.*.cpp")):
            with open(path, "r", encoding="utf-8", errors="replace") as f:
                addrs.update(int(a, 16) for a in rx.findall(f.read()))
        func_set = np.array(sorted(addrs), dtype=np.uint32)
        np.save(cache, func_set)
    print(f"{len(func_set)} recompiled function addresses")

k = C.WinDLL("kernel32", use_last_error=True)
k.OpenProcess.restype = C.c_void_p
k.ReadProcessMemory.argtypes = [C.c_void_p, C.c_void_p, C.c_void_p, C.c_size_t, C.POINTER(C.c_size_t)]
pid = int(subprocess.run(["powershell", "-NoProfile", "-Command", f"(Get-Process {proc})[0].Id"],
                         capture_output=True, text=True).stdout.strip())
hp = k.OpenProcess(0x0410, False, pid)
if not hp:
    sys.exit(f"OpenProcess({pid}) failed: {C.get_last_error()}")


def runs(m):
    """(start, length) of every run of True in a 1-D bool array."""
    if not m.any():
        return []
    d = np.diff(np.concatenate(([0], m.astype(np.int8), [0])))
    starts = np.flatnonzero(d == 1); ends = np.flatnonzero(d == -1)
    return list(zip(starts.tolist(), (ends - starts).tolist()))


CH = 1 << 20
OV = 8192
buf = (C.c_char * (CH + OV))()
addr = scan_lo
found = []
readable = 0
while addr < scan_hi:
    got = C.c_size_t(0)
    ok = k.ReadProcessMemory(hp, C.c_void_p(0x100000000 + addr), buf, CH + OV, C.byref(got))
    if not ok or got.value < 64:
        addr += CH
        continue
    readable += min(got.value, CH)
    words = np.frombuffer(buf.raw[: (got.value // 4) * 4], dtype=">u4")
    mask = (words >= lo) & (words <= hi)
    if mask.any() and func_set is not None:
        mask &= np.isin(words, func_set)
    if mask.any():
        for s, n in runs(mask):
            if n >= minlen and s * 4 < CH:
                found.append(("stride 4", addr + s * 4, 4, [int(x) for x in words[s:s + n]]))
        for phase in range(3):
            sub = mask[phase::3]
            for s, n in runs(sub):
                if n >= minlen:
                    i = phase + s * 3
                    if i * 4 < CH:
                        found.append(("stride 12", addr + i * 4, 12, [int(words[i + e * 3]) for e in range(n)]))
    addr += CH
print(f"scanned {readable >> 20} MB readable")
if within:
    wlo, whi = int(within[0], 16), int(within[1], 16)
    found = [f for f in found if all(wlo <= t <= whi for t in f[3])]
for label, a, step, targets in found:
    # a stride-12 run inside a stride-4 run is the same table: skip it
    if step == 12 and any(l == "stride 4" and b <= a < b + len(t) * 4 for l, b, _, t in found):
        continue
    print(f"{label}: table at guest 0x{a:08X}, {len(targets)} entries")
    for e, t in enumerate(targets[:200]):
        print(f"   [{e:3d}] 0x{a + e * step:08X} -> sub_{t:08X}")
