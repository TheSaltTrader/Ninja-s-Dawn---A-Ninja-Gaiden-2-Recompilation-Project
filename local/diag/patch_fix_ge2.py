"""SUPERSEDED - DO NOT RUN (2026-09-11).
This script turned four `bne` checks (== 2) into `>= 2`. That change was later
found to be the cause of a chapter-end state clamp (the sequence-task state was
forced back to 3 every frame) and was reverted under the marker NG2FIX-CODEGEN,
whose note wrongly blamed the code generator: a fresh codegen emits `!cr6.eq`
at all four sites, which is correct. The real red-mist cause is the profile
award-state wait, fixed by the ng2ChapterAwardFix hook (config/hooks/patches.toml,
src/ng2_chapter_fix.cpp). Kept for the record only.

Original header follows.
Fix chapter 12->13 (and any long post-boss transition): stage-clear detectors
fire the advance only when the clear-timer state == 2. A long post-boss sequence
(video + save + continue prompt) lets the timer count past 2 while the detector
loop is paused, so it is at 3 when the detectors resume and == 2 misses it
forever (the mist). Fix: fire on state >= 2. The check
    cr6.compare<int32_t>(state, 2); if (!cr6.eq) goto <skip>   // skip unless ==2
becomes
    cr6.compare<int32_t>(state, 2); if (cr6.lt) goto <skip>    // skip only if <2

Each check is matched with its compare line for uniqueness (some skip labels are
shared with unrelated branches). Idempotent; --revert restores ==.
"""
import sys

GEN = r"C:\Users\renoi\ClaudeCode\Ninja Gaiden 2 Xbox360\ng2recomp\generated\default"

# (file, compare-register-expr, skip-label)
CHECKS = [
    # sub_82444A90's two checks (loc_82444C54 / loc_82444D1C) were already
    # switched to cr6.lt by the first run; only these two remain.
    (GEN + r"\ng2_recomp.268.cpp", "ctx.r11.s32", "loc_8246A638"),  # sub_8246A500
    (GEN + r"\ng2_recomp.4.cpp", "ctx.r11.s32", "loc_8245B98C"),    # sub_8245B850
]


def main():
    revert = "--revert" in sys.argv
    for path, reg, label in CHECKS:
        base = "cr6.compare<int32_t>(%s, 2, xer);\n\tif (%%s) goto %s;" % (reg, label)
        old = base % "!cr6.eq"
        new = base % "cr6.lt"
        a, b = (new, old) if revert else (old, new)
        with open(path, encoding="utf-8", errors="replace") as fh:
            t = fh.read()
        if b in t and a not in t:
            print("%s: already %s" % (label, "reverted" if revert else "patched"))
            continue
        n = t.count(a)
        if n != 1:
            print("FAIL %s: expected 1, found %d" % (label, n))
            return 1
        t = t.replace(a, b)
        with open(path, "w", encoding="utf-8", newline="") as fh:
            fh.write(t)
        print("%s %s (state>=2)" % ("reverted" if revert else "patched", label))
    return 0


if __name__ == "__main__":
    sys.exit(main())
