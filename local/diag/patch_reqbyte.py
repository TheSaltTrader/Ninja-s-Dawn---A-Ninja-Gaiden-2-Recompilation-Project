"""Log the end-of-chapter sub-state request byte (0x84152737) at the consumer
detector's entry, so the results -> save-prompt -> next-chapter progression (and
where chapter 12 stalls) is visible.

  pt 44  reqbyte at sub_8246A500 entry (deduped) - the sub-state about to run:
         1=results, 2=fire arg0, 3=cleanup, 4=save prompt, 5=fire arg1 (next chapter)

Appended right after the existing CLEAR2 pt-20 line. Idempotent; --revert strips.
"""
import sys

F268 = r"C:\Users\renoi\ClaudeCode\Ninja Gaiden 2 Xbox360\ng2recomp\generated\default\ng2_recomp.268.cpp"
ANCHOR = "\t{ void ng2_diag_xtrace(int, unsigned); ng2_diag_xtrace(20, REX_LOAD_U32(0x83BBBFE0u)); }  // CLEAR2\n"
INJECT = ANCHOR + "\t{ void ng2_diag_xtrace(int, unsigned); ng2_diag_xtrace(44, REX_LOAD_U8(0x84152737u)); }  // REQBYTE\n"


def main():
    revert = "--revert" in sys.argv
    with open(F268, encoding="utf-8", errors="replace") as fh:
        t = fh.read()
    if revert:
        if INJECT in t:
            t = t.replace(INJECT, ANCHOR)
            with open(F268, "w", encoding="utf-8", newline="") as fh:
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
    with open(F268, "w", encoding="utf-8", newline="") as fh:
        fh.write(t)
    print("patched pt44 reqbyte")
    return 0


if __name__ == "__main__":
    sys.exit(main())
