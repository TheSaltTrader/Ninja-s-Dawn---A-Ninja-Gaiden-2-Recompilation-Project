#!/usr/bin/env python3
"""Score const_scan against containers, where the literal block size is KNOWN.

A derivation used on uncontained shaders has to be scored where the truth is
recorded, or it is a guess with a tool around it. Each container states its own
physicalOffset; const_scan never sees it.

Usage: check_const_scan.py <container dir>
"""
import os
import sys
import struct

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from const_scan import scan_min_const


def parse(path):
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
    is_pixel = (flags & 1) == 0
    return po, d[at:at + size], is_pixel


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else "D:/ng2_frameinterp/shaders/xvu"
    files = sorted(f for f in os.listdir(root) if f.endswith(".xvu"))
    exact = over = under = 0
    bad = []
    for name in files:
        r = parse(os.path.join(root, name))
        if r is None:
            continue
        po, code, is_pixel = r
        lowest = scan_min_const(code)
        predicted = 0 if lowest is None else (256 - lowest) * 16
        if predicted == po:
            exact += 1
        elif predicted > po:
            over += 1
            if len(bad) < 10:
                bad.append("OVER  %-26s predicts %5d, actual %5d (lowest c%s, %s)"
                           % (name, predicted, po, lowest, "p" if is_pixel else "v"))
        else:
            under += 1
            if len(bad) < 10:
                bad.append("UNDER %-26s predicts %5d, actual %5d (lowest c%s, %s)"
                           % (name, predicted, po, lowest, "p" if is_pixel else "v"))

    total = exact + over + under
    print("containers scored : %d" % total)
    print("  EXACT           : %d" % exact)
    print("  over-predicted  : %d   (safe: extra unread registers)" % over)
    print("  UNDER-predicted : %d   (UNSAFE: real literals would be missing)" % under)
    for b in bad:
        print("   ", b)
    return 0 if under == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
