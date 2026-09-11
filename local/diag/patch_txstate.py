"""Instrument the chapter-clear driver sub_82442AD8 (479.cpp) to reveal why the
next-chapter step never runs.

Its per-frame state machine switches on ctx.r11 (0..132) and dispatches to handler
labels. The next-stage load (sub_82444530, seqreq + target) is at loc_82442E84,
reached only from certain states. It also yields via rexcrt_SwitchToFiber, so the
machine is a fiber coroutine.

Logs (deduped, id>=10 -> once per distinct value):
  pt 60  state ctx.r11 right before the switch  -> the state trajectory
  pt 61  reached loc_82442E84 (sub_82444530 = LOAD NEXT STAGE)   [value=1]
  pt 62  reached loc_82442E70 (sub_82444408 = seqreq 1)          [value=1]
  pt 63  reached loc_82442E64 (sub_82441450 = mode-128 handover) [value=1]

If pt 60 shows the state climbing then stalling at some N, and pt 61 never logs,
N is the stuck state and the next-stage step is never reached. Idempotent; --revert.
"""
import sys

F = r"C:\Users\renoi\ClaudeCode\Ninja Gaiden 2 Xbox360\ng2recomp\generated\default\ng2_recomp.479.cpp"

EDITS = [
    # (anchor, injected-after-anchor)
    ("\tcr6.compare<uint32_t>(ctx.r11.u32, 132, xer);\n",
     "\t{ void ng2_diag_xtrace(int, unsigned); ng2_diag_xtrace(60, ctx.r11.u32); }  // TXSTATE\n"),
    ("loc_82442E84:\n",
     "\t{ void ng2_diag_xtrace(int, unsigned); ng2_diag_xtrace(61, 1); }  // TXSTATE\n"),
    ("loc_82442E70:\n",
     "\t{ void ng2_diag_xtrace(int, unsigned); ng2_diag_xtrace(62, 1); }  // TXSTATE\n"),
    ("loc_82442E64:\n",
     "\t{ void ng2_diag_xtrace(int, unsigned); ng2_diag_xtrace(63, 1); }  // TXSTATE\n"),
]


def main():
    revert = "--revert" in sys.argv
    with open(F, encoding="utf-8", errors="replace") as fh:
        t = fh.read()
    for anchor, inj in EDITS:
        full = anchor + inj
        if revert:
            if full in t:
                t = t.replace(full, anchor)
            continue
        if full in t:
            continue
        if t.count(anchor) != 1:
            print("FAIL anchor count %d for %r" % (t.count(anchor), anchor[:40]))
            return 1
        t = t.replace(anchor, full)
    with open(F, "w", encoding="utf-8", newline="") as fh:
        fh.write(t)
    print("reverted" if revert else "instrumented sub_82442AD8 (pt 60-63)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
