"""Chapter 12->13 permanent-fix candidate: guard the transition-slot command-list
scan against a corrupt list pointer.

Crash chain (named via the live function table):
  sub_82442AD8 -> sub_82445440 (scans 32 transition slots, base ~0x83CDF018 stride
  584, acts on slots whose [+0]==3) -> sub_82445918 -> sub_82467E70. sub_82467E70
  is a tagged-command-list scanner (tags 0x80000018 / 0x80000003); its list pointer
  is r3 = [slot+476]. For the ch12->13 handover one slot's [+476] is a bad pointer
  (~guest 0x51000, uncommitted low memory) -> first read faults -> "game over" +
  crash. The clean save-reload path does NOT go through this, which is why it works.

Fix: in sub_82445918, before calling sub_82467E70, skip the scan when [slot+476] is
not a plausible guest pointer (< 0x00100000 -- no resource command list lives that
low; valid lists are in loaded archive/heap memory). General: any slot with a
corrupt list pointer is skipped rather than crashing the transition. If the
transition then completes to chapter 13, this is the permanent fix.

Diagnostics (deduped >=10): pt 50 slot ptr, pt 51 [slot+448], pt 52 [slot+476]
(the list ptr), pt 53 [slot+480]. pt 5 (id<10, every time) = GUARD SKIPPED a slot.

Idempotent; --revert strips it. Re-run after codegen regeneration.
"""
import sys

F = r"C:\Users\renoi\ClaudeCode\Ninja Gaiden 2 Xbox360\ng2recomp\generated\default\ng2_recomp.457.cpp"

ANCHOR = (
    "\tctx.r3.u64 = REX_LOAD_U32(r31.u32 + 476);\n"
    "\t// bl 0x82467e70\n"
    "\tsub_82467E70(ctx, base);\n"
)
INJECT = (
    "\tctx.r3.u64 = REX_LOAD_U32(r31.u32 + 476);\n"
    "\t// bl 0x82467e70  -- NG2FIX-SLOTGUARD: skip a transition slot whose command-list pointer is corrupt\n"
    "\t{\n"
    "\t\tvoid ng2_diag_xtrace(int, unsigned);\n"
    "\t\tng2_diag_xtrace(50, r31.u32);\n"
    "\t\tng2_diag_xtrace(51, REX_LOAD_U32(r31.u32 + 448));\n"
    "\t\tng2_diag_xtrace(52, ctx.r3.u32);\n"
    "\t\tng2_diag_xtrace(53, REX_LOAD_U32(r31.u32 + 480));\n"
    "\t\tif (ctx.r3.u32 < 0x00100000u) {\n"
    "\t\t\tng2_diag_xtrace(5, ctx.r3.u32);   // GUARD: corrupt list ptr, skip scan\n"
    "\t\t} else {\n"
    "\t\t\tsub_82467E70(ctx, base);\n"
    "\t\t}\n"
    "\t}\n"
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
    print("patched slotguard")
    return 0


if __name__ == "__main__":
    sys.exit(main())
