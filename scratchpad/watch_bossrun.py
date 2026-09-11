# Boss-run watcher (2026-09-11 session 13, elevated + vision).
# Usage: python watch_bossrun.py <PID> [outfile]
# Polls the transition globals ~4x/sec and prints every change with a timestamp.
# On every change of the STATE KEY (MODE/clr/en/g1/menu/LOADSTATE) it also diffs the
# 32 transition-effect slots (base 0x83CDF018, stride 584: [+0] state, [+476] list ptr,
# [+24]) against the previous snapshot, so we learn whether any slot carries the mist
# (clean gameplay vs post-boss mist vs after-the-fix faint-mist gameplay).
# Also logs the MODE=2 pending-count array 0x85008474..80, a7 (commit flag), wipeE4,
# and how long inp has been frozen (the hang signature).
import sys, time
sys.path.insert(0, __file__.rsplit("\\", 1)[0])
from rpm import snap, u32be, u16be, u8

SLOT_BASE = 0x83CDF018; SLOT_STRIDE = 584; NSLOTS = 32
out = open(sys.argv[2], "a", buffering=1) if len(sys.argv) > 2 else None

def log(line):
    print(line, flush=True)
    if out: out.write(line + "\n")

def slots():
    r = []
    for i in range(NSLOTS):
        s = SLOT_BASE + i * SLOT_STRIDE
        r.append((u32be(s), u32be(s + 476), u32be(s + 24), u32be(s + 4)))
    return r

def extras():
    return dict(a7=u8(0x84C250A7), wipeE4=u32be(0x83BBBFE4),
                cnt=[u32be(0x85008474 + 4 * k) for k in range(4)])

def fmt(s): return " ".join(f"{k}={('?' if v is None else hex(v))}" for k, v in s.items())

prev_key = None; prev_slots = slots(); prev_inp = None; frozen = 0
t0 = time.time()
log(f"=== watch_bossrun start {time.strftime('%H:%M:%S')} pid={sys.argv[1]} ===")
log(f"[  0.0s] initial slots: " + " ".join(f"{i}:{st:#x}/{p:#x}" for i, (st, p, _, _) in enumerate(prev_slots)))
while True:
    try:
        s = snap(); e = extras()
    except Exception as ex:
        log(f"[{time.time()-t0:6.1f}s] read error: {ex}"); time.sleep(0.25); continue
    if s["MODE"] is None:
        log(f"[{time.time()-t0:6.1f}s] reads returning None (process gone / crashed?)"); time.sleep(1); continue
    frozen = frozen + 1 if s["inp"] == prev_inp else 0
    prev_inp = s["inp"]
    key = (s["MODE"], s["clr"], s["en"], s["g1"], s["menu"], s["LOADSTATE"], e["a7"], tuple(e["cnt"]))
    if key != prev_key:
        log(f"[{time.time()-t0:6.1f}s {time.strftime('%H:%M:%S')}] {fmt(s)} a7={e['a7']} wipeE4={e['wipeE4']:#x} cnt={e['cnt']} inp_frozen={frozen}")
        cur = slots()
        diff = [(i, prev_slots[i], cur[i]) for i in range(NSLOTS) if prev_slots[i] != cur[i]]
        if diff:
            log("      slot diff: " + "; ".join(f"{i}: st {a[0]:#x}->{b[0]:#x} p476 {a[1]:#x}->{b[1]:#x} +24 {a[2]:#x}->{b[2]:#x} +4 {a[3]:#x}->{b[3]:#x}" for i, a, b in diff))
        prev_slots = cur
        prev_key = key
    elif frozen and frozen % 120 == 0:   # every ~30s of a frozen inp, remind
        log(f"[{time.time()-t0:6.1f}s] inp frozen for {frozen} polls ({fmt(s)})")
    time.sleep(0.25)
