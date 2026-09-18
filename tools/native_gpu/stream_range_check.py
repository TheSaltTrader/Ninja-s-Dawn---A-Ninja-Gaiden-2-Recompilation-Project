#!/usr/bin/env python3
"""Check every NGPU_STREAM index in translated HLSL against the array that holds it.

A CORRECTNESS GATE, not a count of successes - which is most of what this
project's instruments have been.

    fable2_shader_common.h:
      uint4 g_StreamSlots[4] : packoffset(c44);            // SIXTEEN entries
      #define NGPU_STREAM(s) g_VertexStreamHeap[g_StreamSlots[(s) >> 2][(s) & 3]]

So a valid index is 0..15. NGPU_STREAM(94) reads g_StreamSlots[23], past the end
of a 4-element array. The index is a COMPILE-TIME CONSTANT, and dxc compiles it
without complaint: the shader loads, the pipeline builds, the draw issues, and
every per-stage instrument passes it. The sibling project found ten such
references in one shader this way, in a container written the same day and
sitting in the cache its renderer draws with.

The general point, which is why this file exists: agreement between two decoders
of the same format is nearly worthless, because both were written from the same
documentation and fail the same way. Agreement between a decoder and the
COMPILED OUTPUT is strong, because the compiler had no access to the decoder's
assumptions. My own vfetch census claimed eighteen fetch slots spanning 0..95;
the HLSL it was supposed to describe contained exactly two stream indices, and
that contradiction is what exposed the census rather than any amount of reading
it back.

ALSO FLAGS A VERTEX SHADER THAT REFERENCES NO STREAM AT ALL. Under constraint 9
every translated vertex shader reads geometry through ngpu_vload against the
stream heap, so a vertex shader with no NGPU_STREAM is either not a vertex
shader or fetches nothing - and "it translated" would not have told anyone
which. Reported separately rather than folded into a pass/fail, because a
shader that legitimately computes its positions is a real case.
"""
import os
import re
import sys

STREAM = re.compile(r"NGPU_STREAM\((\d+)\)")
# The array is uint4[4]; a shader is a vertex shader if it declares one.
VS_HINT = re.compile(r"\bvoid\s+main\s*\(|SV_Position|ngpu_vload")

MAX_SLOT = 16


def main():
    dirs = sys.argv[1:] or [r"D:\ng2_frameinterp\shaders\out_fetchless\hlsl"]
    files = refs = bad_refs = 0
    bad_files = []
    no_stream_vs = []
    hist = {}
    for d in dirs:
        if not os.path.isdir(d):
            print(f"no such directory: {d}")
            continue
        for name in sorted(os.listdir(d)):
            if not name.endswith(".hlsl"):
                continue
            files += 1
            src = open(d + os.sep + name, encoding="utf-8", errors="replace").read()
            found = [int(m) for m in STREAM.findall(src)]
            for s in found:
                refs += 1
                hist[s] = hist.get(s, 0) + 1
                if s >= MAX_SLOT:
                    bad_refs += 1
            if any(s >= MAX_SLOT for s in found):
                bad_files.append((name, sorted({s for s in found if s >= MAX_SLOT})))
            # A VERTEX shader with no stream reference. "contains ngpu_vload"
            # does NOT identify one - the shared header DEFINES that function,
            # so every shader carries the string, pixel shaders included. The
            # first version of this check flagged 685 of 964 files on exactly
            # that basis and the number meant nothing.
            #
            # The corpus names pixel containers with a _p suffix, which is the
            # only discriminator here that is not a guess about content.
            if not found and not name.endswith("_p.hlsl"):
                no_stream_vs.append(name)

    print(f"files scanned        : {files}")
    print(f"NGPU_STREAM refs     : {refs}")
    print(f"  indices in use     : {', '.join(str(k) for k in sorted(hist))}"
          if hist else "  indices in use     : (none)")
    print(f"OUT OF RANGE (>= {MAX_SLOT}) : {bad_refs}"
          + ("   <-- reads past g_StreamSlots" if bad_refs else ""))
    for name, idx in bad_files[:20]:
        print(f"    {name}  ->  {idx}")
    if no_stream_vs:
        print(f"uses ngpu_vload but references NO stream: {len(no_stream_vs)}")
        for n in no_stream_vs[:10]:
            print(f"    {n}")
    print()
    print("PASS" if bad_refs == 0 else f"FAIL: {bad_refs} out-of-range reference(s)")
    return 0 if bad_refs == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
