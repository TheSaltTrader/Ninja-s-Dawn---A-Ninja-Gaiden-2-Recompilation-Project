"""Chapter 12->13 handover recovery (the "red mist" fix).

Root cause (established over prior sessions):
  The end-of-chapter handover fires only when a stage-clear DETECTOR observes the
  global clear-timer (0x83BBBFE0) at state==2 AND its own "stage loaded" gate is
  open. Chapter 12's post-boss sequence is long enough that a per-frame ticker
  (sub_836FB348, called from 40+ sites on the SAME global timer) counts the timer
  past state 2 to state 3 while every detector's gate is closed. Once at state 3,
  no detector ever fires sub_82441A80, so chapter 13 never loads -> the mist hangs.

Fix (general, save-independent):
  sub_836FB348 is the per-frame timer tick. At its entry we watch the completion:
  when the clear-timer has reached state 3 (done) but the handover MODE (0x84C28870)
  is not already engaged (!=128) and no sequencer chapter-change is pending
  (seqreq 0x84C288AC == 0), the handover was missed -> fire sub_82441A80(0) now and
  drop the timer to idle (state 0) so it fires exactly once.

Why it is safe on normal chapters:
  On a chapter that hands over correctly, the detector fires sub_82441A80 while the
  timer is at state 2; that sets mode=128 and, a few frames later, seqreq!=0, and
  the next chapter loads (resetting the timer) BEFORE it reaches state 3. So the
  (state==3 && mode!=128 && seqreq==0) condition is only ever true when the handover
  was actually missed. sub_836FB2B0 only re-inits on state==1, so parking at 0 does
  not disturb the next chapter's own clear timer.

Addresses (verified in generated code):
  0x83BBBFE0  clear-timer state   (sub_836FB348: lis -31812; addi -16416)
  0x84C28870  handover mode = 128 (sub_82441A80: [-2067660800 + 20592] = 128)
  0x84C288AC  sequencer request   (sub_8364A830: [-2067660800 + 20652], !=0 -> load)
  0x84152737  detector request byte (diagnostic only)

Diagnostics (deduped, id>=10): pt 43 timer state, pt 41 mode, pt 42 seqreq,
  pt 40 request byte at the moment of firing. pt 2 (id<10, every time) = FIX FIRED.

Idempotent; --revert strips it. Re-run after codegen regeneration.
"""
import sys

GEN = r"C:\Users\renoi\ClaudeCode\Ninja Gaiden 2 Xbox360\ng2recomp\generated\default"
CPP = GEN + r"\ng2_recomp.502.cpp"
HDR = GEN + r"\ng2_funcs.502.h"

CPP_ANCHOR = "DEFINE_REX_FUNC(sub_836FB348) {\n\tREX_FUNC_PROLOGUE();\n"
CPP_BLOCK = (
    "\t// NG2FIX-HANDOVER: recover a missed chapter handover (ch12->13 red mist).\n"
    "\t{\n"
    "\t\tvoid ng2_diag_xtrace(int, unsigned);\n"
    "\t\tunsigned _st   = REX_LOAD_U32(0x83BBBFE0u);   // clear-timer state\n"
    "\t\tunsigned _mode = REX_LOAD_U32(0x84C28870u);   // handover mode (128=in progress)\n"
    "\t\tunsigned _sreq = REX_LOAD_U32(0x84C288ACu);   // sequencer chapter-change request\n"
    "\t\tng2_diag_xtrace(43, _st);\n"
    "\t\tng2_diag_xtrace(41, _mode);\n"
    "\t\tng2_diag_xtrace(42, _sreq);\n"
    "\t\tif (_st == 3u && _mode != 128u && _sreq == 0u) {\n"
    "\t\t\tng2_diag_xtrace(40, REX_LOAD_U8(0x84152737u));\n"
    "\t\t\tng2_diag_xtrace(2, 0);                       // FIX FIRED\n"
    "\t\t\tREX_STORE_U32(0x83BBBFE0u, 0u);              // consume: park timer idle\n"
    "\t\t\tctx.r3.u64 = 0;\n"
    "\t\t\tsub_82441A80(ctx, base);\n"
    "\t\t\treturn;\n"
    "\t\t}\n"
    "\t}\n"
)
CPP_INJECT = CPP_ANCHOR + CPP_BLOCK

HDR_ANCHOR = '#include "ng2_pch.h"\n'
HDR_INJECT = HDR_ANCHOR + "\nDECLARE_REX_FUNC(sub_82441A80);  // NG2FIX-HANDOVER\n"


def patch(path, anchor, inject, revert):
    with open(path, encoding="utf-8", errors="replace") as fh:
        t = fh.read()
    if revert:
        if inject in t:
            t = t.replace(inject, anchor)
            with open(path, "w", encoding="utf-8", newline="") as fh:
                fh.write(t)
            print("reverted", path)
        else:
            print("not patched:", path)
        return 0
    if inject in t:
        print("already patched:", path)
        return 0
    if t.count(anchor) != 1:
        print("FAIL %s: anchor count %d" % (path, t.count(anchor)))
        return 1
    t = t.replace(anchor, inject)
    with open(path, "w", encoding="utf-8", newline="") as fh:
        fh.write(t)
    print("patched", path)
    return 0


def main():
    revert = "--revert" in sys.argv
    rc = patch(HDR, HDR_ANCHOR, HDR_INJECT, revert)
    if rc:
        return rc
    return patch(CPP, CPP_ANCHOR, CPP_INJECT, revert)


if __name__ == "__main__":
    sys.exit(main())
