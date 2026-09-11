"""Instrument the chapter-clear detector (sub_82444A90) to find why chapter 12
never fires the advance.

  pt 1   sub_82441A80 entry            - the advance actually fired (rare, logged every time)
  pt 10  clear-timer state [r31+0]     - the detector's countdown state, deduped

sub_82444A90 reads the clear-timer state at three points and, when it reaches 2,
calls sub_82441A80. Logging the state (deduped) shows how far it gets: for a
chapter that advances it should climb to 2; for chapter 12 it will stall, and
where it stalls is the bug. Scoped to sub_82444A90's line range so the identical
load in other functions is untouched.

Idempotent; --revert strips it. Re-run after codegen regeneration.
"""
import re
import sys

GEN = r"C:\Users\renoi\ClaudeCode\Ninja Gaiden 2 Xbox360\ng2recomp\generated\default"
F70 = GEN + r"\ng2_recomp.70.cpp"
F405 = GEN + r"\ng2_recomp.405.cpp"

STATE_LOAD = "\tctx.r11.u64 = REX_LOAD_U32(r31.u32 + 0);"
STATE_INJECT = ("\tctx.r11.u64 = REX_LOAD_U32(r31.u32 + 0);\n"
                "\t{ void ng2_diag_xtrace(int, unsigned); ng2_diag_xtrace(10, ctx.r11.u32); }  // CLEARTRACE")
ENTRY_ANCHOR = "DEFINE_REX_FUNC(sub_82441A80) {\n\tREX_FUNC_PROLOGUE();\n"
ENTRY_INJECT = (ENTRY_ANCHOR +
                "\t{ void ng2_diag_xtrace(int, unsigned); ng2_diag_xtrace(1, 0); }  // CLEARTRACE\n")


def func_range(text, name):
    m = re.search(r"DEFINE_REX_FUNC\(%s\)\s*\{" % re.escape(name), text)
    start = m.start()
    nxt = text.find("\nDEFINE_REX_FUNC(", m.end())
    return start, (nxt if nxt > 0 else len(text))


def main():
    revert = "--revert" in sys.argv
    # --- 70.cpp: instrument state loads only inside sub_82444A90 ---
    with open(F70, encoding="utf-8", errors="replace") as fh:
        t = fh.read()
    if revert:
        n = t.count(STATE_INJECT)
        t = t.replace(STATE_INJECT, STATE_LOAD)
        with open(F70, "w", encoding="utf-8", newline="") as fh:
            fh.write(t)
        print("70.cpp: reverted %d state-load site(s)" % n)
    elif "CLEARTRACE" in t:
        print("70.cpp: already patched")
    else:
        s, e = func_range(t, "sub_82444A90")
        body = t[s:e]
        count = body.count(STATE_LOAD)
        body = body.replace(STATE_LOAD, STATE_INJECT)
        t = t[:s] + body + t[e:]
        with open(F70, "w", encoding="utf-8", newline="") as fh:
            fh.write(t)
        print("70.cpp: instrumented %d clear-timer state load(s) in sub_82444A90" % count)

    # --- 405.cpp: sub_82441A80 entry ---
    with open(F405, encoding="utf-8", errors="replace") as fh:
        t = fh.read()
    if revert:
        if ENTRY_INJECT in t:
            t = t.replace(ENTRY_INJECT, ENTRY_ANCHOR)
            with open(F405, "w", encoding="utf-8", newline="") as fh:
                fh.write(t)
            print("405.cpp: reverted entry")
        else:
            print("405.cpp: entry not patched")
    elif ENTRY_INJECT in t:
        print("405.cpp: already patched")
    else:
        if t.count(ENTRY_ANCHOR) != 1:
            print("405.cpp: FAIL, entry anchor count %d" % t.count(ENTRY_ANCHOR))
            return 1
        t = t.replace(ENTRY_ANCHOR, ENTRY_INJECT)
        with open(F405, "w", encoding="utf-8", newline="") as fh:
            fh.write(t)
        print("405.cpp: instrumented sub_82441A80 entry")
    return 0


if __name__ == "__main__":
    sys.exit(main())
