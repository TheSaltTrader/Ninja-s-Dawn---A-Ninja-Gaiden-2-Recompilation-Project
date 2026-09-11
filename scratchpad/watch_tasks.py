# Task-table + transition watcher (session 14). Works on the recomp AND on Xenia (both map guest
# memory at host 0x100000000 + guest, big-endian).
# Usage: python watch_tasks.py <PID> [outfile]
# Every ~4 Hz it logs any change of: MODE/clr/en/g1/menu-state/next-state/LOADSTATE/seq/a7/b6/
# inp, the front-end button byte 0x84C39A5A, the confirmation ctx state (0x83B26D29), and the
# 24-entry task table at 0x83BF1C98 (state, countdown, work fn) - so the moment a task is
# activated or parked shows up with a timestamp next to the screen the player is on.
import sys, time
sys.path.insert(0, __file__.rsplit("\\", 1)[0])
from rpm import rd, u32be, u16be, u8, snap

out = open(sys.argv[2], "a", buffering=1) if len(sys.argv) > 2 else None
def log(line):
    print(line, flush=True)
    if out: out.write(line + "\n")

def tasks():
    r = []
    for i in range(24):
        t = 0x83BF1C98 + i * 28
        r.append((u32be(t), u32be(t + 4), u32be(t + 12)))
    return r

def extras():
    return dict(a7=u8(0x84C250A7), b6=u8(0x84C250B6), btn=u8(0x84C39A5A), cst=u8(0x83B26D29),
                nxt=u8(0x842DC326), B=u32be(0x84A53268), C=u8(0x84289349),
                seqreq=u32be(0x84C250AC), flag=u32be(0x84AED970))

def fmt(d): return " ".join(f"{k}={('?' if v is None else hex(v))}" for k, v in d.items())

t0 = time.time()
log(f"=== watch_tasks start {time.strftime('%H:%M:%S')} pid={sys.argv[1]} ===")
prev_key = None; prev_t = None
while True:
    try:
        s = snap(); e = extras(); tk = tasks()
    except Exception as ex:
        log(f"[{time.time()-t0:6.1f}s] read error: {ex}"); time.sleep(0.5); continue
    if s["MODE"] is None:
        log(f"[{time.time()-t0:6.1f}s] reads returning None (process gone?)"); time.sleep(1); continue
    key = (tuple(s.values()), tuple(e.values()))
    if key != prev_key:
        log(f"[{time.time()-t0:6.1f}s {time.strftime('%H:%M:%S')}] {fmt(s)} {fmt(e)}")
        prev_key = key
    if prev_t is None:
        log("      tasks: " + " ".join(f"{i}:{st}/{cnt}/{fn:08x}" for i, (st, cnt, fn) in enumerate(tk) if st or fn))
    else:
        diff = [(i, prev_t[i], tk[i]) for i in range(24) if prev_t[i] != tk[i]]
        if diff:
            log(f"[{time.time()-t0:6.1f}s {time.strftime('%H:%M:%S')}] task diff: " + "; ".join(
                f"{i}: st {a[0]}->{b[0]} cnt {a[1]}->{b[1]} fn {a[2]:08x}->{b[2]:08x}" for i, a, b in diff))
    prev_t = tk
    time.sleep(0.25)
