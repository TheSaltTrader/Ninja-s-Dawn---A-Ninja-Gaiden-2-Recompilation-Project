"""Instrument the REAL transition driver sub_83633E88 (384.cpp) - the front-end
sequencer scene state machine, confirmed live via a mode-word watchpoint caller
chain (0x8380E638 SwitchToFiber -> sub_822F2C70 -> sub_836331B0 -> sub_837E10C8 ->
sub_83633E88 -> the mode writers). It dispatches on a U16 state [r31+0] (0..152)
and is stuck writing gameplay mode 3 instead of advancing to the load-next-chapter
state.

  pt 65  the state [r31+0] at the switch (deduped) -> the state trajectory through
         a chapter clear; where it stops climbing is the stuck state.

Idempotent; --revert. No watchpoint/debugger, so it won't crash the cutscene.
"""
import sys

F = r"C:\Users\renoi\ClaudeCode\Ninja Gaiden 2 Xbox360\ng2recomp\generated\default\ng2_recomp.384.cpp"
ANCHOR = "\tcr6.compare<uint32_t>(ctx.r11.u32, 152, xer);\n"
INJECT = ANCHOR + "\t{ void ng2_diag_xtrace(int, unsigned); ng2_diag_xtrace(65, ctx.r11.u32); }  // SEQSTATE\n"


def main():
    revert = "--revert" in sys.argv
    t = open(F, encoding="utf-8", errors="replace").read()
    if revert:
        if INJECT in t:
            open(F, "w", encoding="utf-8", newline="").write(t.replace(INJECT, ANCHOR))
            print("reverted")
        else:
            print("not patched")
        return 0
    if INJECT in t:
        print("already patched")
        return 0
    if t.count(ANCHOR) != 1:
        print("FAIL anchor count %d" % t.count(ANCHOR))
        return 1
    open(F, "w", encoding="utf-8", newline="").write(t.replace(ANCHOR, INJECT))
    print("instrumented sub_83633E88 state (pt 65)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
