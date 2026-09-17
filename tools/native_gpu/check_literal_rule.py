#!/usr/bin/env python3
"""Does the literal-block rule hold on NG2's containers, or is it Fable-specific?

The Fable II session derived, exceptionless over its 65 real VERTEX containers:

    physicalOffset == (256 - firstLiteralRegister) * 16
    psize          == physicalOffset + size

MEASURED HERE: the first is vertex-only. Xenos' constant file holds 512 float4s,
vertex shaders in the low half and pixel shaders in the high half, and literals
anchor to the top of whichever half the shader lives in. So the constant is 256
for a vertex shader and 512 for a pixel one. Applied unchanged to a pixel shader
the Fable form predicts a NEGATIVE offset - c508 gives -4032 where the truth is
64. On NG2's 625 containers: 161 failures with 256, ZERO once generalised.

which rests on literals always occupying the TOP of the constant file, ending at
c255. That is what makes an UNCONTAINED shader's literal block derivable from its
microcode alone - find the lowest high-range constant register the program reads
and the offset follows.

Every rule this week that was generalised from one title's population has needed
re-measuring on the other's, and four of them were wrong. The distributions
already differ here (NG2 13.4% at offset 0 against Fable's 29.2%; offset 128 is
2 of 625 here against 5 of 65 there), so the question is not idle.

This checks the rule where the truth is KNOWN - against containers that carry a
definition table - because a rule that fails on containered shaders cannot be
trusted on uncontained ones, and the uncontained ones are the whole point.

Usage: check_literal_rule.py <container dir>
"""
import os
import struct
import sys


def parse(path):
    """Return (physicalOffset, size, psize, [ (reg, count, offset) ]) or None."""
    d = open(path, "rb").read()
    if len(d) < 0x24:
        return None
    flags, vsize, psize = struct.unpack_from(">3I", d, 0)
    if (flags & 0xFFFFFF00) != 0x102A1100:
        return None
    dtoff, shoff = struct.unpack_from(">I", d, 0x14)[0], struct.unpack_from(">I", d, 0x18)[0]
    if shoff + 8 > len(d):
        return None
    po, size = struct.unpack_from(">2I", d, shoff)

    defs = []
    if dtoff:
        i = dtoff + 20
        # The table is terminated by a zero dword. Bound the walk: a malformed
        # or absent table must not be read as a very long one.
        while i + 8 <= len(d) and len(defs) < 64:
            w = struct.unpack_from(">I", d, i)[0]
            if w == 0:
                break
            reg, cnt = struct.unpack_from(">2H", d, i)
            phys = struct.unpack_from(">I", d, i + 4)[0]
            defs.append((reg, cnt, phys))
            i += 8
    return po, size, psize, defs


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else r"D:\ng2_frameinterp\shaders\xvu"
    files = sorted(f for f in os.listdir(root) if f.endswith(".xvu"))
    if not files:
        print("no containers in", root)
        return 2

    parsed = offset_rule_ok = offset_rule_bad = 0
    psize_rule_ok = psize_rule_bad = 0
    no_defs = 0
    by_offset = {}
    bad_examples = []

    for name in files:
        r = parse(os.path.join(root, name))
        if r is None:
            continue
        po, size, psize, defs = r
        parsed += 1
        by_offset[po] = by_offset.get(po, 0) + 1

        # psize == physicalOffset + size
        if psize == po + size:
            psize_rule_ok += 1
        else:
            psize_rule_bad += 1
            if len(bad_examples) < 8:
                bad_examples.append("%s: psize %d != po %d + size %d" % (name, psize, po, size))

        if not defs:
            no_defs += 1
            # No literals declared: the rule predicts offset 0. Check that too,
            # because "no table" and "offset 0" must agree or the rule is leaky.
            if po == 0:
                offset_rule_ok += 1
            else:
                offset_rule_bad += 1
                if len(bad_examples) < 8:
                    bad_examples.append("%s: no definitions but physicalOffset %d" % (name, po))
            continue

        first_reg = min(reg for reg, _, _ in defs)
        # THE CONSTANT FILE IS 512 FLOAT4s, NOT 256 - vertex shaders occupy the
        # low half and pixel shaders the high half. Fable's rule was derived from
        # 65 VERTEX containers, so it reads "256" and is silently vertex-only; on
        # a pixel shader it predicts a NEGATIVE offset. The top of the file is
        # what literals are anchored to, so the constant is whichever half this
        # shader lives in.
        top = 512 if first_reg > 255 else 256
        predicted = (top - first_reg) * 16
        if predicted == po:
            offset_rule_ok += 1
        else:
            offset_rule_bad += 1
            if len(bad_examples) < 8:
                bad_examples.append("%s: first literal c%d predicts %d, actual physicalOffset %d"
                                    % (name, first_reg, predicted, po))

    print("containers parsed: %d of %d files" % (parsed, len(files)))
    print("physicalOffset distribution: %s"
          % ", ".join("%d->%d" % (k, v) for k, v in sorted(by_offset.items())))
    print("  %d carry no definition table" % no_defs)
    print()
    print("RULE 1  physicalOffset == (TOP - firstLiteralRegister) * 16,")
    print("        TOP = 256 for a vertex shader, 512 for a pixel shader")
    print("        holds %d, FAILS %d" % (offset_rule_ok, offset_rule_bad))
    print("RULE 2  psize == physicalOffset + size")
    print("        holds %d, FAILS %d" % (psize_rule_ok, psize_rule_bad))
    if bad_examples:
        print("\ncounterexamples (up to 8):")
        for e in bad_examples:
            print("   ", e)
    else:
        print("\nno counterexamples - the rule is not Fable-specific.")
    return 0 if (offset_rule_bad == 0 and psize_rule_bad == 0) else 1


if __name__ == "__main__":
    sys.exit(main())
