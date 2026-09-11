"""Full boss->mist transition listener. Polls the key chapter-end state at ~50 Hz
and logs every CHANGE with a timestamp. Works on BOTH our recompile and Xenia
(guest membase 0x100000000 for both), so the two runs can be diffed directly to
find where the state progression diverges.

    python local/diag/watch_transition.py <pid> [seconds]

Watched guest addresses (the transition state machine):
  0x83BBBFE0 u32  clear-timer state (1 measure / 2 count / 3 done)
  0x84152737 u8   detector request byte (sub-state: 1 results..4 save..5 next)
  0x84C25070 u32  MODE word (3 gameplay; 129/130 results/save; 131/132 next chapter)
  0x84C250AC u32  sequencer request (nonzero -> load)
  0x8531F3D0 u32  SAVE-SCENE state ([r30+2720]; must reach 2 to advance) <-- KEY
  0x842DC325 u8   sequencer state (4 in-game)
"""
import ctypes, ctypes.wintypes as wt, struct, sys, time

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
k32.OpenProcess.restype = wt.HANDLE
k32.ReadProcessMemory.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p,
                                  ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]

WATCH = [
    (0x83BBBFE0, 4, "timer"),
    (0x84152737, 1, "reqbyte"),
    (0x84C25070, 4, "MODE"),
    (0x84C250AC, 4, "seqreq"),
    (0x8531F3D0, 4, "SAVESCENE"),
    (0x842DC325, 1, "seqstate"),
]


def read(h, host, n):
    buf = (ctypes.c_ubyte * n)()
    got = ctypes.c_size_t()
    if k32.ReadProcessMemory(h, ctypes.c_void_p(host), buf, n, ctypes.byref(got)) and got.value == n:
        return int.from_bytes(bytes(buf), "big")
    return None


def find_membase(h):
    for cand in (0x100000000, 0x200000000, 0x300000000):
        if read(h, cand + 0x82000000, 4):
            return cand
    return None


def main():
    pid = int(sys.argv[1])
    dur = float(sys.argv[2]) if len(sys.argv) > 2 else 180.0
    h = k32.OpenProcess(0x0410, False, pid)  # QUERY | VM_READ
    if not h:
        print("OpenProcess failed", ctypes.get_last_error()); return 1
    base = find_membase(h)
    if base is None:
        print("no guest membase"); return 1
    print("pid %d membase 0x%X  watching %d values for %.0fs" % (pid, base, len(WATCH), dur))
    print("%-13s %-9s %-11s %s" % ("t(s)", "field", "old", "new"))
    last = {}
    t0 = time.time()
    while time.time() - t0 < dur:
        for va, n, name in WATCH:
            v = read(h, base + va, n)
            if v is None:
                v = "<unmapped>"
            if name not in last:
                last[name] = v
                print("%-13.3f %-9s %-11s 0x%X (init)" % (time.time() - t0, name, "-", v) if isinstance(v, int)
                      else "%-13.3f %-9s %-11s %s (init)" % (time.time() - t0, name, "-", v))
            elif last[name] != v:
                op = ("0x%X" % last[name]) if isinstance(last[name], int) else str(last[name])
                np = ("0x%X" % v) if isinstance(v, int) else str(v)
                print("%-13.3f %-9s %-11s %s" % (time.time() - t0, name, op, np))
                last[name] = v
        time.sleep(0.02)
    print("done")
    return 0


if __name__ == "__main__":
    sys.exit(main())
