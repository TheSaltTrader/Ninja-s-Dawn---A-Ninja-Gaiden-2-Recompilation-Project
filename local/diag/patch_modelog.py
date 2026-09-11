"""Overnight diagnostic: pin exactly where the chapter-end advance chain breaks.

Established: the transition driver sub_82442AD8 dispatches on the MODE word
0x84C25070; only mode 131/132 route to the next-chapter load (sub_82444530). The
mode is stuck at 3. Mode 131/132 is set by sub_823EAD48/sub_823EE560/sub_8245F548,
called by the results/save updaters sub_823EE428 (gated on [r30+2720]==2) and
sub_823EAC98 (unconditional) right AFTER rexcrt_SwitchToFiber.

Logs (deduped id>=10 unless noted):
  pt 70  every MODE write value (0x84C25070) -> the mode sequence through a clear
  pt 74  sub_823EE560 entry (advance to 131)        [value 1]
  pt 75  sub_823EAD48 entry (advance to 131)        [value 1]
  pt 76  sub_8245F548 entry (advance to 132)        [value 1]
  pt 77  sub_823EE428 entry (save-scene updater)    [value 1]
  pt 78  sub_823EAC98 entry (continue-scene updater)[value 1]
  pt 79  sub_823EE428 reached the [r30+2720] gate AFTER SwitchToFiber; value=[r30+2720]
  pt 80  sub_823EAC98 reached code AFTER SwitchToFiber [value 1]

Reading it: if pt77/78 never fire, the updater scene isn't reached. If they fire but
pt79/80 never do, SwitchToFiber never returns (fiber stall = root cause). If pt79
fires with value!=2, the gate is the block. If pt74/75/76 fire but mode 131/132 never
appears in pt70, the setter is being clobbered. Idempotent; --revert.
"""
import re, sys

GEN = r"C:\Users\renoi\ClaudeCode\Ninja Gaiden 2 Xbox360\ng2recomp\generated\default"

# --- 1. log every mode write, in every file that has one ---
MODE_STORE = re.compile(r"(\tREX_STORE_U32\((?:ctx\.r\d+|r\d+)\.u32 \+ 20592, ([^)]+)\);\n)")
MODE_TAG = "  // MODELOG"


def do_mode_writes(revert):
    import glob, os
    n = 0
    for path in glob.glob(os.path.join(GEN, "ng2_recomp.*.cpp")):
        t = open(path, encoding="utf-8", errors="replace").read()
        if revert:
            if MODE_TAG in t:
                t = re.sub(r"\t\{ void ng2_diag_xtrace\(int, unsigned\); ng2_diag_xtrace\(70, [^)]+\); \}" + re.escape(MODE_TAG) + r"\n", "", t)
                open(path, "w", encoding="utf-8", newline="").write(t)
            continue
        if MODE_TAG in t:
            continue
        def repl(m):
            val = m.group(2)
            return m.group(1) + "\t{ void ng2_diag_xtrace(int, unsigned); ng2_diag_xtrace(70, %s); }%s\n" % (val, MODE_TAG)
        t2, k = MODE_STORE.subn(repl, t)
        if k:
            open(path, "w", encoding="utf-8", newline="").write(t2)
            n += k
    print(("reverted" if revert else "instrumented") + " %d mode writes" % n)


# --- 2. entry markers for the advance chain functions ---
ENTRY = [
    ("ng2_recomp.390.cpp", "sub_823EE560", 74),
    ("ng2_recomp.210.cpp", "sub_823EAD48", 75),
    ("ng2_recomp.435.cpp", "sub_8245F548", 76),
    ("ng2_recomp.115.cpp", "sub_823EE428", 77),
    ("ng2_recomp.161.cpp", "sub_823EAC98", 78),
]

# after rexcrt_SwitchToFiber in sub_823EE428: does the fiber return, and is the
# [r30+2720]==2 gate met? Log the gate value the frame the fiber hands back.
GATE_ANCHOR = ("\trexcrt_SwitchToFiber(ctx, base);\n"
               "\tctx.r11.u64 = REX_LOAD_U32(r30.u32 + 2720);\n")
GATE_INJECT = (GATE_ANCHOR +
               "\t{ void ng2_diag_xtrace(int, unsigned); ng2_diag_xtrace(79, ctx.r11.u32); }  // MODELOG\n")


def do_gate(revert):
    path = GEN + r"\ng2_recomp.115.cpp"
    t = open(path, encoding="utf-8", errors="replace").read()
    if revert:
        if GATE_INJECT in t:
            open(path, "w", encoding="utf-8", newline="").write(t.replace(GATE_INJECT, GATE_ANCHOR))
        return
    if GATE_INJECT in t:
        return
    if t.count(GATE_ANCHOR) != 1:
        print("WARN gate anchor count %d" % t.count(GATE_ANCHOR))
        return
    open(path, "w", encoding="utf-8", newline="").write(t.replace(GATE_ANCHOR, GATE_INJECT))
    print("gate probe (pt 79) added")


def anchor(fn):
    return "DEFINE_REX_FUNC(%s) {\n\tREX_FUNC_PROLOGUE();\n" % fn


def do_entries(revert):
    for fname, fn, pt in ENTRY:
        path = GEN + "\\" + fname
        t = open(path, encoding="utf-8", errors="replace").read()
        a = anchor(fn)
        inj = a + "\t{ void ng2_diag_xtrace(int, unsigned); ng2_diag_xtrace(%d, 1); }  // MODELOG\n" % pt
        if revert:
            if inj in t:
                open(path, "w", encoding="utf-8", newline="").write(t.replace(inj, a))
            continue
        if inj in t:
            continue
        if t.count(a) != 1:
            print("WARN entry anchor %s count %d" % (fn, t.count(a)))
            continue
        open(path, "w", encoding="utf-8", newline="").write(t.replace(a, inj))
        print("entry marked", fn)


def main():
    revert = "--revert" in sys.argv
    do_mode_writes(revert)
    do_entries(revert)
    do_gate(revert)
    return 0


if __name__ == "__main__":
    sys.exit(main())
