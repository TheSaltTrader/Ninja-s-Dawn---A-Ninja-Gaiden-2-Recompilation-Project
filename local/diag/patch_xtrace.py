"""Instrument the chapter-transition trigger points with ng2_diag_xtrace().

Adds calls at the five points that fire only at a stage transition, so one
chapter-12 boss run shows which (if any) the game reaches after the continue:

  pt 1  sub_82441A80 entry     - stage-end / pause flow entry (sets mode 128)
  pt 2  sub_82441450 req-store  - the mode-128 flow writes the sequencer request
  pt 3  sub_82444530 req-store  - the "continue/quit" path writes the request
  pt 4  sub_82444408 req-store  - another request writer
  pt 6  sub_8245F358 entry     - the actual stage loader runs

ng2_diag_xtrace is defined in src/diag_hooks.cpp. Idempotent; run --revert to
strip the calls. Re-run after any codegen regeneration.
"""
import sys

GEN = r"C:\Users\renoi\ClaudeCode\Ninja Gaiden 2 Xbox360\ng2recomp\generated\default"

# (file, anchor, injected-snippet-that-follows-anchor)
POINTS = [
    (GEN + r"\ng2_recomp.405.cpp",
     "DEFINE_REX_FUNC(sub_82441A80) {\n\tREX_FUNC_PROLOGUE();\n",
     "\t{ void ng2_diag_xtrace(int, unsigned); ng2_diag_xtrace(1, 0); }  // XTRACE\n"),
    (GEN + r"\ng2_recomp.280.cpp",
     "\t// stw r11,20652(r10)\n\tREX_STORE_U32(ctx.r10.u32 + 20652, ctx.r11.u32);",
     None),  # store-site: handled specially below (inject BEFORE the store)
    (GEN + r"\ng2_recomp.325.cpp",
     "\t// stw r11,20652(r10)\n\tREX_STORE_U32(ctx.r10.u32 + 20652, ctx.r11.u32);",
     None),
    (GEN + r"\ng2_recomp.165.cpp",
     "\t// stw r11,20652(r10)\n\tREX_STORE_U32(ctx.r10.u32 + 20652, ctx.r11.u32);",
     None),
    (GEN + r"\ng2_recomp.86.cpp",
     "DEFINE_REX_FUNC(sub_8245F358) {\n\tREX_FUNC_PROLOGUE();\n",
     "\t{ void ng2_diag_xtrace(int, unsigned); ng2_diag_xtrace(6, 0); }  // XTRACE\n"),
]

STORE_IDS = {"280": 2, "325": 3, "165": 4}
STORE_ANCHOR = "\t// stw r11,20652(r10)\n\tREX_STORE_U32(ctx.r10.u32 + 20652, ctx.r11.u32);"


def build_edits():
    edits = []
    # entry points 1 and 6
    for path, anchor, snippet in POINTS:
        if snippet is None:
            continue
        edits.append((path, anchor, anchor + snippet))
    # store points 2,3,4 - inject the xtrace BEFORE the store, logging r11
    for fp, idv in STORE_IDS.items():
        path = GEN + ("\\ng2_recomp.%s.cpp" % fp)
        new = ("\t// stw r11,20652(r10)\n"
               "\t{ void ng2_diag_xtrace(int, unsigned); ng2_diag_xtrace(%d, ctx.r11.u32); }  // XTRACE\n"
               "\tREX_STORE_U32(ctx.r10.u32 + 20652, ctx.r11.u32);" % idv)
        edits.append((path, STORE_ANCHOR, new))
    # point 5 - the sequencer (sub_8364A830) sees a non-zero advance request,
    # regardless of which function wrote it. Logs the value.
    p5_old = ("\tif (cr6.eq) goto loc_8364A874;\n"
              "\t// bl 0x8364fd48\n"
              "\tsub_8364FD48(ctx, base);")
    p5_new = ("\tif (cr6.eq) goto loc_8364A874;\n"
              "\t{ void ng2_diag_xtrace(int, unsigned); ng2_diag_xtrace(5, ctx.r11.u32); }  // XTRACE seq sees request\n"
              "\t// bl 0x8364fd48\n"
              "\tsub_8364FD48(ctx, base);")
    edits.append((GEN + r"\ng2_recomp.390.cpp", p5_old, p5_new))
    return edits


def main():
    revert = "--revert" in sys.argv
    edits = build_edits()
    changed = 0
    for path, old, new in edits:
        with open(path, encoding="utf-8", errors="replace") as fh:
            txt = fh.read()
        if revert:
            if new in txt:
                txt = txt.replace(new, old)
                with open(path, "w", encoding="utf-8", newline="") as fh:
                    fh.write(txt)
                print("REVERTED: %s" % path)
                changed += 1
            else:
                print("SKIP (not patched): %s" % path)
            continue
        if new in txt:
            print("SKIP (already patched): %s" % path)
            continue
        n = txt.count(old)
        if n != 1:
            print("FAIL: %s expected 1 anchor, found %d" % (path, n))
            return 1
        txt = txt.replace(old, new)
        with open(path, "w", encoding="utf-8", newline="") as fh:
            fh.write(txt)
        print("OK: instrumented %s" % path)
        changed += 1
    print("done, %d file(s) changed" % changed)
    return 0


if __name__ == "__main__":
    sys.exit(main())
