#!/usr/bin/env python3
"""Does each container's code region hold INSTRUCTIONS, or a float table?

From the sibling project, which found twelve containers whose dumped "code"
was IEEE floats - the literal block captured where the microcode should be:

    421521D0_p.cpu.xvu  3f52796e 3ea6e005 be6c5232  =  0.82, 0.326, -0.23
    421521D0_p.xvu      00011002 00001200 c4000000  =  Xenos control flow

Both 404 bytes, identical headers, same code offset and size; one stable, one
not. A translator fed non-instructions wanders off the end and reads whatever
follows, and the signature is unmistakable once you look for it: an identical
prefix, then divergence from the FIRST emitted instruction onward.

That matters because it is indistinguishable from translator non-determinism
unless you test for it. The test is cheap: real Xenos microcode scores 20-47%
plausible-float by chance; a float table scores ~100%.

NG2's 625 containers: 0 above 90%, median 24%, max 48% - the extractor here
points at real code. A clean negative, and worth keeping runnable so it stays
one.

Usage: check_code_is_code.py [container dir]
"""
import os
import statistics
import struct
import sys


def code_region(path):
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
    return d[at:at + size]


def float_fraction(code):
    n = len(code) // 4
    ok = tot = 0
    for i in range(n):
        v = struct.unpack_from(">I", code, i * 4)[0]
        if v == 0:
            continue
        tot += 1
        f = struct.unpack(">f", struct.pack(">I", v))[0]
        if f != f or f in (float("inf"), float("-inf")):
            continue
        if 1e-6 <= abs(f) <= 1e6:
            ok += 1
    return (ok / tot) if tot else 0.0


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else "D:/ng2_frameinterp/shaders/xvu"
    scores = []
    for n in sorted(os.listdir(root)):
        if not n.endswith(".xvu"):
            continue
        c = code_region(os.path.join(root, n))
        if c is not None:
            scores.append((float_fraction(c), n, len(c)))
    if not scores:
        print("no containers parsed in %s - refusing to report" % root)
        return 2
    bad = sorted((s for s in scores if s[0] > 0.90), reverse=True)
    vals = [s[0] for s in scores]
    print("containers scored       : %d" % len(scores))
    print("code region >90%% floats : %d  <-- these hold a literal block, not code" % len(bad))
    for f, n, ln in bad[:10]:
        print("    %-26s %.0f%% float-like, %d bytes" % (n, f * 100, ln))
    print("distribution: min %.0f%%  median %.0f%%  max %.0f%%"
          % (min(vals) * 100, statistics.median(vals) * 100, max(vals) * 100))
    print("(real microcode lands 20-47%% by chance; a float table scores ~100%%)")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
