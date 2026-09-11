# Watch the red-mist chapter transition and report whether the mode-3 fix produced a CLEAN load.
# Usage: python watch_transition.py <PID>
# Poll the key globals ~4x/sec; print every change with a timestamp. Interpretation:
#   * At the post-boss mist you'll see: clr=0x3 en=0xFFFF g1=0xFF MODE=3, inp FROZEN.
#   * The fix firing shows: menu->3 then clr->1, then MODE walks 3->1->2 (streaming, inp resumes).
#   * CLEAN full fix  = MODE reaches 2->0->2->3 AND en ends 0x3  (mist cleared, aurora bg rendered).
#   * Playable+faint  = MODE forced 2->3 by the 106.cpp safety net; en may not reach 0x3 (faint mist).
#   * FAIL/stall      = MODE sticks at 1 or 2 for >8s with no further change.
#   * CRASH           = reads start returning None / values zero out (MODE/inp/en all 0).
import sys, time
sys.path.insert(0, __file__.rsplit("\\",1)[0])
from rpm import snap

def fmt(s): return " ".join(f"{key}={('?' if v is None else hex(v))}" for key,v in s.items())
prev = None
t0 = time.time()
print("watching (Ctrl+C to stop)...")
while True:
    try:
        s = snap()
    except Exception as e:
        print(f"[{time.time()-t0:6.1f}s] read error: {e}"); time.sleep(0.25); continue
    key = (s["MODE"], s["clr"], s["en"], s["g1"], s["menu"], s["LOADSTATE"])
    if key != prev:
        print(f"[{time.time()-t0:6.1f}s] {fmt(s)}")
        prev = key
    time.sleep(0.25)
