"""Ground-truth transition capture. Polls the FULL chapter-end gate chain at ~60 Hz
and logs every CHANGE with a timestamp. Works on BOTH our recompile (ng2.exe) and
Xenia (xenia_canary.exe) -- guest membase 0x100000000 for both -- so a working Xenia
transition can be diffed against our (dormant) one to find the FIRST activation write
we skip.

    python -u local/diag/watch_transition2.py <pid|name> [seconds]

Addresses (all verified by lis*65536 arithmetic, not hand hex):
  0x84C25070 u32  MODE           (3 gameplay; 128 handover; 131/132 next chapter)
  0x84C250AC u32  seqreq         (nonzero -> loader sub_8364A830)
  0x852E14E4 u32  counter_5348   (>0 -> save-scene updater sub_823EE428)   <-- gate
  0x852E14EC u32  counter_5356   (>0 -> other results branch)              <-- gate
  0x83B26D24 u32  frontend_state (sub_823E2078 dispatch: 0 idle / 1,2,3 active) <-- KEY
  0x8531F3D0 u32  SAVESCENE      ([r30+2720]; must reach 2)
  0x84152737 u8   reqbyte        (save/results sub-state; 4 = save prompt)
  0x83BBBFE0 u32  clear_timer    (1 measure / 2 count / 3 done)
  0x842DC325 u8   seqstate       (4 in-game)

In OUR build every one of these stays at its idle value through the whole ending.
In Xenia, the one(s) that change -- and the ORDER -- reveal the activation path.
"""
import ctypes, ctypes.wintypes as wt, sys, time

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
k32.OpenProcess.restype = wt.HANDLE
k32.ReadProcessMemory.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p,
                                  ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]

WATCH = [
    (0x84C25070, 4, "MODE"),
    (0x84C250AC, 4, "seqreq"),
    (0x852E14E4, 4, "cnt5348"),
    (0x852E14EC, 4, "cnt5356"),
    (0x83B26D24, 4, "frontend_state"),
    (0x8531F3D0, 4, "SAVESCENE"),
    (0x84152737, 1, "reqbyte"),
    (0x83BBBFE0, 4, "clear_timer"),
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


def resolve_pid(arg):
    try:
        return int(arg)
    except ValueError:
        pass
    # by name via tasklist
    import subprocess
    name = arg if arg.lower().endswith(".exe") else arg + ".exe"
    out = subprocess.check_output(["tasklist", "/fi", "imagename eq " + name, "/fo", "csv", "/nh"],
                                  text=True, errors="replace")
    pids = []
    for line in out.splitlines():
        parts = [p.strip('"') for p in line.split('","')]
        if len(parts) >= 2 and parts[0].lower() == name.lower():
            pids.append(int(parts[1]))
    if not pids:
        print("no process named", name); sys.exit(1)
    if len(pids) > 1:
        print("multiple pids for", name, pids, "- using", pids[0])
    return pids[0]


def main():
    if len(sys.argv) < 2:
        print(__doc__); return 1
    pid = resolve_pid(sys.argv[1])
    dur = float(sys.argv[2]) if len(sys.argv) > 2 else 300.0
    h = k32.OpenProcess(0x0410, False, pid)  # QUERY | VM_READ
    if not h:
        print("OpenProcess failed", ctypes.get_last_error()); return 1
    base = find_membase(h)
    if base is None:
        print("no guest membase (is the game past the boot screen?)"); return 1
    print("pid %d membase 0x%X  watching %d values for %.0fs (Ctrl-C to stop)" %
          (pid, base, len(WATCH), dur))
    print("%-11s %-15s %-12s %s" % ("t(s)", "field", "old", "new"))
    last = {}
    t0 = time.time()
    hb = t0
    while time.time() - t0 < dur:
        for va, n, name in WATCH:
            v = read(h, base + va, n)
            if v is None:
                v = "<unmapped>"
            if name not in last:
                last[name] = v
                sv = ("0x%X" % v) if isinstance(v, int) else v
                print("%-11.3f %-15s %-12s %s (init)" % (time.time() - t0, name, "-", sv))
            elif last[name] != v:
                op = ("0x%X" % last[name]) if isinstance(last[name], int) else str(last[name])
                np = ("0x%X" % v) if isinstance(v, int) else str(v)
                print("%-11.3f %-15s %-12s %s" % (time.time() - t0, name, op, np))
                last[name] = v
        now = time.time()
        if now - hb >= 15.0:
            hb = now
            print("%-11.3f (heartbeat: MODE=%s frontend=%s seqreq=%s)" %
                  (now - t0,
                   ("0x%X" % last.get("MODE")) if isinstance(last.get("MODE"), int) else last.get("MODE"),
                   ("0x%X" % last.get("frontend_state")) if isinstance(last.get("frontend_state"), int) else last.get("frontend_state"),
                   ("0x%X" % last.get("seqreq")) if isinstance(last.get("seqreq"), int) else last.get("seqreq")))
        time.sleep(0.016)
    print("done")
    return 0


if __name__ == "__main__":
    sys.exit(main())
