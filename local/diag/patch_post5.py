"""Chapter 12->13 in-place fix (timing): after the save-prompt sub-state (4) is set
up, immediately queue the next-chapter handover sub-state (5) instead of going idle.

The ch12 end sequence reaches sub-state 4 (save prompt) but never posts sub-state 5,
so it hangs at the mist. Forcing 5 with an EXTERNAL poke ~20s later fails: by then the
transition-effect slots are freed (null list pointers), and the handover cannot
complete (mode 128 set but seqreq 0x84C250AC never fires -> game over / hang). The
natural handover on ch1-11 fires DURING the transition, while that state is alive.

So post sub-state 5 in-code the frame after sub-state 4 finishes. sub-state 4 is
loc_8246A91C in sub_8246A500 (268.cpp); it falls through to the shared loc_8246ABA0
which does reqbyte[0x84152737]=r28(0). We redirect ONLY the sub-state-4 fall-through:
set reqbyte=5 and return, leaving loc_8246ABA0 (reached by the invalid-substate bail)
to still reset to 0. r26 = 0x84150000 (reqbyte base) throughout; the epilogue matches
loc_8246ABA4 (addi r1,r1,720; return). The four scanner guards remain as a backstop.

Idempotent; --revert strips it. Re-run after codegen regeneration.
"""
import sys

F = r"C:\Users\renoi\ClaudeCode\Ninja Gaiden 2 Xbox360\ng2recomp\generated\default\ng2_recomp.268.cpp"

ANCHOR = (
    "\t// stb r10,20646(r11)\n"
    "\tREX_STORE_U8(ctx.r11.u32 + 20646, ctx.r10.u8);\n"
    "loc_8246ABA0:\n"
)
INJECT = (
    "\t// stb r10,20646(r11)\n"
    "\tREX_STORE_U8(ctx.r11.u32 + 20646, ctx.r10.u8);\n"
    "\t// NG2FIX-POST5: save prompt (sub-state 4) done -> queue sub-state 5 now, before teardown\n"
    "\t{ void ng2_diag_xtrace(int, unsigned); ng2_diag_xtrace(3, 5);\n"
    "\t  REX_STORE_U8(r26.u32 + 10039, (uint8_t)5); ctx.r1.s64 = ctx.r1.s64 + 720; return; }\n"
    "loc_8246ABA0:\n"
)


def main():
    revert = "--revert" in sys.argv
    with open(F, encoding="utf-8", errors="replace") as fh:
        t = fh.read()
    if revert:
        if INJECT in t:
            t = t.replace(INJECT, ANCHOR)
            with open(F, "w", encoding="utf-8", newline="") as fh:
                fh.write(t)
            print("reverted")
        else:
            print("not patched")
        return 0
    if INJECT in t:
        print("already patched")
        return 0
    if t.count(ANCHOR) != 1:
        print("FAIL: anchor count %d" % t.count(ANCHOR))
        return 1
    t = t.replace(ANCHOR, INJECT)
    with open(F, "w", encoding="utf-8", newline="") as fh:
        fh.write(t)
    print("patched post5")
    return 0


if __name__ == "__main__":
    sys.exit(main())
