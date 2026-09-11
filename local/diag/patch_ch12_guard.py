"""Stop the chapter-12 guard from forcing the early return inside chapter 12.

Byte-level patch (the file holds a literal NUL in a char constant, so text
tools are not safe on it). Each replacement must match exactly once, and the
file is only written when every one does.
"""
import io
import sys

P = r"C:\Users\renoi\ClaudeCode\Ninja Gaiden 2 Xbox360\ng2recomp\src\patch_hooks.cpp"
src = io.open(P, "rb").read()

edits = [
    (
        b"""  // The community patch's behaviour, but only where it belongs.
  //
  // Its author warns it causes problems OUTSIDE chapter 12, which is why it was
  // a global switch nobody could sensibly leave on. The file-open hook already
  // knows which chapter is loading - the game opens s_chap_NN.ng2 for it - so
  // the workaround can be scoped to chapter 12 and released everywhere else,
  // which is what makes it safe to have on by default.
  //
  // The cvar remains as an unconditional override for anyone who wants the
  // original always-on behaviour.
  const bool in_chapter_12 = ng2::CurrentChapter() == 12;
  const bool force = REXCVAR_GET(ng2_chapter12_workaround) || in_chapter_12;
  if (!force) {""",
        b"""  // The pointer is TESTED, never assumed bad - in chapter 12 as everywhere.
  //
  // This used to force the community patch's early return throughout chapter
  // 12 ("scoped to where it belongs"). That was the chapter 12 -> 13 hang.
  // 2026-09-06 12:50:19: the boss died, the site was called with a NEW
  // pointer, 0xF0B72AE0 - a real object in mapped guest memory (+0x30 held a
  // sane heap pointer) - and the forced return threw its work away. The game
  // then sat in the mist forever, still answering input, because the state
  // that starts chapter 13 had been skipped. Its author's warning that the
  // patch "causes problems" was this. The crash the patch exists for is a
  // bad-pointer dereference, and the readability check below is the right
  // guard for that: a pointer that can be read is a pointer the game may use.
  //
  // The cvar remains as an unconditional override for anyone who wants the
  // original always-on behaviour.
  const bool force = REXCVAR_GET(ng2_chapter12_workaround);
  if (!force) {""",
    ),
    (
        b"""                va, in_chapter_12 ? " (chapter 12)"
                                  : (force ? " (forced)" : " - not readable"));""",
        b"""                va, force ? " (forced by ng2_chapter12_workaround)" : " - not readable");""",
    ),
]

out = src
for old, new in edits:
    n = out.count(old)
    if n != 1:
        print("FAIL: expected exactly one match, found %d for:\n%s" % (n, old[:120].decode("ascii", "replace")))
        sys.exit(1)
    out = out.replace(old, new)

io.open(P, "wb").write(out)
print("patched %s: %d replacements, %d -> %d bytes" % (P, len(edits), len(src), len(out)))
