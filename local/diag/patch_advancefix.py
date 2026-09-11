"""CANDIDATE FIX (do NOT apply until the morning modelog run confirms the path).

Hypothesis: the SAVE-flow results updater sub_823EE428 (115.cpp) reaches the mode-131
advance only if the save-scene state [r30+2720] == 2, and our recompile's save scene
never reports 2, so it takes the no-advance branch and the mode falls back to 3 (mist).

This patch relaxes gate 1 so the advance (sub_823EE560 -> mode 131 -> next chapter)
runs regardless of [r30+2720]. mode-131 is set unconditionally inside sub_823EE560, so
this triggers the next-chapter load.

APPLY ONLY IF the morning trace shows: pt77 (sub_823EE428) fires AND pt79 shows a
stable value != 2 AND pt74 (sub_823EE560) never fires. If instead pt78 (sub_823EAC98,
the unconditional path) is active and still doesn't advance, the block is the fiber
return, NOT this gate -- do not use this patch; investigate rexcrt_SwitchToFiber.

Risk: advances the frame the save scene starts, i.e. may auto-skip the save prompt and
may fire before the save-scene setup is complete (the earlier forced-handover attempts
game-overed for a related reason). If it game-overs, the save scene needs to run to its
real terminal state first -- prefer fixing why [r30+2720] never becomes 2 over relaxing.

Idempotent; --revert.
"""
import sys

F = r"C:\Users\renoi\ClaudeCode\Ninja Gaiden 2 Xbox360\ng2recomp\generated\default\ng2_recomp.115.cpp"

ANCHOR = (
    "\tcr6.compare<int32_t>(ctx.r11.s32, 2, xer);\n"
    "\t// bne cr6,0x823ee4bc\n"
    "\tif (!cr6.eq) goto loc_823EE4BC;\n"
)
INJECT = (
    "\tcr6.compare<int32_t>(ctx.r11.s32, 2, xer);\n"
    "\t// bne cr6,0x823ee4bc  -- NG2FIX-ADVANCE: relaxed; run the mode-131 advance regardless of save-scene state\n"
    "\tif (false) goto loc_823EE4BC;\n"
)


def main():
    revert = "--revert" in sys.argv
    t = open(F, encoding="utf-8", errors="replace").read()
    if revert:
        if INJECT in t:
            open(F, "w", encoding="utf-8", newline="").write(t.replace(INJECT, ANCHOR))
            print("reverted")
        else:
            print("not applied")
        return 0
    if INJECT in t:
        print("already applied")
        return 0
    if t.count(ANCHOR) != 1:
        print("FAIL anchor count %d" % t.count(ANCHOR))
        return 1
    open(F, "w", encoding="utf-8", newline="").write(t.replace(ANCHOR, INJECT))
    print("applied gate-relax candidate")
    return 0


if __name__ == "__main__":
    sys.exit(main())
