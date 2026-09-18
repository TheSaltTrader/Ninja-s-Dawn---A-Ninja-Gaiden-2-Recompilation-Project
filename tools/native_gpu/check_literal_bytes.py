#!/usr/bin/env python3
"""Score literal_bytes() against what each container DECLARES, on the corpus.

Replaces check_const_scan.py, which imports `scan_min_const` - the "lowest
constant register" approach that was abandoned when it returned c0 for
essentially every shader. The check script was never updated, so it has not
imported successfully since, and the 625/625 figure quoted for the literal rule
has had no runnable validation behind it for some time. An unrunnable check and
a passing check are indistinguishable from the outside, which is the same
property this project has spent the day finding in other instruments.

GROUND TRUTH: a real container states its own answer. The code sits at
`vsize + physicalOffset`, so the bytes between the start of the physical section
and the code ARE the literal block - and physicalOffset is read from the file,
not derived. So for every container the declared size is known exactly, and the
rule can be scored rather than believed.

REPORTS BOTH DIRECTIONS SEPARATELY, because they are not equally bad:
  UNDER  the shader loses literals it needs and computes with zeros;
  OVER   a declared block is baked in as LOCALS, REPLACING a correct
         g_Consts(N) read - so over-declaring corrupts constants that were
         being read correctly. The sibling project established that; it is why
         "over-declaring is safe" was withdrawn.

And it prints the TRIVIAL BASELINE beside the score, because NG2's blocks are
0 or 64 bytes with two exceptions - so "0 if c255 unread else 64" scores about
623/625 and any rule that merely learns the corpus skew scores well.
"""
import os
import struct
import sys

from const_scan import literal_bytes


def parse(path):
    """Return (code, declared_literal_bytes, is_pixel) or None."""
    d = open(path, "rb").read()
    if len(d) < 0x24:
        return None
    flags, vsize, psize = struct.unpack_from(">3I", d, 0)
    if (flags & 0xFFFFFF00) != 0x102A1100:
        return None
    shoff = struct.unpack_from(">I", d, 0x18)[0]
    if shoff + 8 > len(d):
        return None
    po, size = struct.unpack_from(">2I", d, shoff)
    at = vsize + po
    if at + size > len(d) or size == 0:
        return None
    return d[at:at + size], po, (flags & 1) == 0


def main():
    d = sys.argv[1] if len(sys.argv) > 1 else r"D:\ng2_frameinterp\shaders\xvu"
    exact = under = over = 0
    unparsed = 0
    trivial_exact = 0
    hist = {}
    worst = []
    for name in sorted(os.listdir(d)):
        if not name.endswith(".xvu"):
            continue
        r = parse(os.path.join(d, name))
        if r is None:
            unparsed += 1
            continue
        code, declared, is_pixel = r
        hist[declared] = hist.get(declared, 0) + 1
        got = literal_bytes(code)
        if got == declared:
            exact += 1
        elif got < declared:
            under += 1
            worst.append((name, declared, got, "UNDER"))
        else:
            over += 1
            worst.append((name, declared, got, "OVER"))
        # The trivial rule: 0 when the rule says 0, else 64.
        triv = 0 if got == 0 else 64
        if triv == declared:
            trivial_exact += 1

    total = exact + under + over
    print(f"containers scored : {total}   (unparseable {unparsed})")
    print(f"  EXACT           : {exact}")
    print(f"  UNDER-predicted : {under}   (shader loses literals it needs)")
    print(f"  OVER-predicted  : {over}   (bakes locals over correct g_Consts reads)")
    if total:
        print(f"  rule            : {100.0 * exact / total:.1f}%")
        print(f"  TRIVIAL BASELINE: {100.0 * trivial_exact / total:.1f}%"
              f"  ({trivial_exact}/{total}, '0 or 64')")
    print()
    print("declared literal-block sizes in this corpus:")
    for k in sorted(hist):
        print(f"  {k:4d} bytes : {hist[k]}")
    if worst:
        print()
        print("disagreements (first 12):")
        for w in worst[:12]:
            print(f"  {w[0]}  declared {w[1]}  rule {w[2]}  {w[3]}")


if __name__ == "__main__":
    sys.exit(main() or 0)
