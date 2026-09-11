"""Chapter 12->13 permanent fix: null-guard the transition-effect command-list
scanners at their entry.

The end-of-chapter handover walks a 32-slot transition-effect table (sub_82445440)
and processes each active slot (sub_82445918, sub_82445D68), which feed the slot's
several command-list pointers ([slot+472/476/480/488]) to list-scanner functions.
By the time the ch12->13 handover runs, those effect lists have been FREED, so the
pointers are null. The scanners read [listptr+0] with NO null check, then walk
upward looking for a terminator; from a null/low pointer they run off the committed
low region (0..0x3F000) into unmapped memory and fault reading ~guest 0x51000 ->
"game over" + crash. On hardware an empty/null list is simply a no-op.

Fix: at each scanner's entry, if the list pointer is not a plausible guest pointer
(< 0x00100000 -- no real command list lives that low; valid lists are loaded
resource memory, high addresses), return immediately (empty list). Guarding the
FUNCTIONS (not the call sites) covers every caller. Each scanner takes its list
pointer in a different register:
    sub_82467E70  list = r3
    sub_82467FF8  list = r4   (reads [r4+0] before its own -1 terminator check)

Diagnostics: pt 6 = sub_82467E70 guarded, pt 7 = sub_82467FF8 guarded (id<10, every
time). Idempotent; --revert strips it. Re-run after codegen regeneration.
"""
import sys

import os
# generated/default, located from this file so the script works from any checkout.
GEN = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                    "..", "..", "generated", "default"))

# (file, function, list-pointer register expr, point-id)
# The complete transition command-list scanner family reachable from sub_82445440
# that scans-until-terminator (0x80000018 / 0x80000003 / -1) and so runs off a
# null/low pointer into unmapped memory. Found by static call-graph sweep +
# terminator-pattern filter (local/diag find_scanners); the counted-loop and
# already-null-checked candidates were excluded.
SCANNERS = [
    (GEN + r"\ng2_recomp.267.cpp", "sub_82467E70", "ctx.r3.u32", 6),
    (GEN + r"\ng2_recomp.325.cpp", "sub_82467FF8", "ctx.r4.u32", 7),
    (GEN + r"\ng2_recomp.448.cpp", "sub_82467D68", "ctx.r4.u32", 8),
    (GEN + r"\ng2_recomp.408.cpp", "sub_82484658", "ctx.r3.u32", 9),
]


def anchor(func):
    return "DEFINE_REX_FUNC(%s) {\n\tREX_FUNC_PROLOGUE();\n" % func


def inject(func, reg, pt):
    return (anchor(func) +
            "\t// NG2FIX-SCANGUARD: a null/low command-list pointer is an empty list; do not scan it.\n"
            "\tif (%s < 0x00100000u) { void ng2_diag_xtrace(int, unsigned); ng2_diag_xtrace(%d, %s); return; }\n"
            % (reg, pt, reg))


def main():
    revert = "--revert" in sys.argv
    for path, func, reg, pt in SCANNERS:
        with open(path, encoding="utf-8", errors="replace") as fh:
            t = fh.read()
        a, inj = anchor(func), inject(func, reg, pt)
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
        print("guarded %s (list=%s, pt %d)" % (func, reg, pt))
    return 0


if __name__ == "__main__":
    sys.exit(main())
