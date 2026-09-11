"""Instrument the entry of all three advance-firers to log the clear-timer
state they observe, so we see which one runs while the timer is at state 2.

  pt 22  sub_82444A90 entry  - clear-timer state [0x83BBBFE0] on entry
  pt 20  sub_8246A500 entry  - same
  pt 30  sub_8245B850 entry  - same

The clear-timer completes (state 3) but the advance never fires. During the ~54s
the timer counts at state 2, whichever detector runs should catch it. Logging the
timer state each detector sees (deduped) reveals which detector runs at state 2
and therefore which one is failing to fire sub_82441A80. Reads the timer struct
directly at 0x83BBBFE0 so no register plumbing is needed.

Idempotent; --revert strips it.
"""
import sys

GEN = r"C:\Users\renoi\ClaudeCode\Ninja Gaiden 2 Xbox360\ng2recomp\generated\default"

# (file, function, point-id)
POINTS = [
    (GEN + r"\ng2_recomp.70.cpp", "sub_82444A90", 22),
    (GEN + r"\ng2_recomp.268.cpp", "sub_8246A500", 20),
    (GEN + r"\ng2_recomp.4.cpp", "sub_8245B850", 30),
]


def anchor(func):
    return "DEFINE_REX_FUNC(%s) {\n\tREX_FUNC_PROLOGUE();\n" % func


def inject(func, pt):
    return (anchor(func) +
            "\t{ void ng2_diag_xtrace(int, unsigned); ng2_diag_xtrace(%d, REX_LOAD_U32(0x83BBBFE0u)); }  // CLEAR2\n" % pt)


def main():
    revert = "--revert" in sys.argv
    for path, func, pt in POINTS:
        with open(path, encoding="utf-8", errors="replace") as fh:
            t = fh.read()
        a, inj = anchor(func), inject(func, pt)
        if revert:
            if inj in t:
                t = t.replace(inj, a)
                with open(path, "w", encoding="utf-8", newline="") as fh:
                    fh.write(t)
                print("reverted %s" % func)
            else:
                print("%s not patched" % func)
            continue
        if inj in t:
            print("%s already patched" % func)
            continue
        if t.count(a) != 1:
            print("FAIL %s: anchor count %d" % (func, t.count(a)))
            return 1
        t = t.replace(a, inj)
        with open(path, "w", encoding="utf-8", newline="") as fh:
            fh.write(t)
        print("instrumented %s (pt %d)" % (func, pt))
    return 0


if __name__ == "__main__":
    sys.exit(main())
