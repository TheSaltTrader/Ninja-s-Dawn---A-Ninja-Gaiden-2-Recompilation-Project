#!/usr/bin/env python3
"""Rebuild every FETCHLESS container with a derived vertex element table.

THE BUG THIS WORKS AROUND. XenosRecomp does, on the main path of every vertex
shader that fetches anything (shader_recompiler.cpp:180):

    auto findResult = vertexElements.find(address);
    assert(findResult != vertexElements.end());
    ...
    findResult->second.usage

`vertexElements` is built by looping vertexElementCount times over the
container's element table, so a FETCHLESS container - vertexElementCount == 0 -
leaves it EMPTY, find() returns end(), and the assert is compiled out because
the shipped recompiler is built /O2 /Ob2 /DNDEBUG. It then dereferences the end
iterator.

Measured consequences, one bug wearing two faces:
  - the byte read as `usage` indexes USAGE_SEMANTICS, so the emitted input name
    is whatever happened to be there. Ten runs of one container gave two
    outputs, identical in size, differing in five lines: iBinormal0 against
    iPosition0. Always a valid name - so one of the two is the WRONG shader
    rather than a differently-spelled one;
  - the same dereference segfaults when the memory is less kind, which is every
    "recompiler failed" this project attributed first to the shaders and then to
    machine load.

83.5% of NG2's vertex containers are fetchless, so this is the majority case
here, not a corner.

THE FIX WITHOUT TOUCHING THE SHARED TOOL: give the container an element table.
synth_xvu derives one by scanning for vfetch instructions, which populates the
map and makes the lookup hit. Predicted, then measured - ten synthesised
containers, six runs each: 1 distinct output, 6/6 produced, zero failures,
against 2 distinct outputs and intermittent crashes from the real containers
carrying the same microcode.

The semantics are invented (TexCoord0..N) and that is sound here for the reason
synth_xvu's own docstring gives: the native path binds vertex buffers from fetch
constants and builds its input layout from this same translation's sidecar, so
they only have to be unique and self-consistent, not true. A container that
already HAS a table is left alone - its semantics are real and better.

Usage: synth_fetchless.py <container dir> <out dir>
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import synth_xvu
from vfetch_scan import scan


def parse(path):
    """(code, is_pixel, vertex_element_count) or None."""
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
    count = 0
    if not is_pixel and shoff + 0x20 <= len(d):
        count = struct.unpack_from(">I", d, shoff + 0x1C)[0]
    # The literal block sits immediately BEFORE the code, and psize covers both,
    # so it is recoverable from the container itself rather than needing the
    # runtime dump - unlike an uncontained shader.
    preamble = d[vsize:vsize + po] if po else None
    return d[at:at + size], is_pixel, count, preamble


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    src, out = sys.argv[1], sys.argv[2]
    os.makedirs(out, exist_ok=True)

    made = left_alone = pixel = no_fetch = unparsed = 0
    for name in sorted(os.listdir(src)):
        if not name.endswith(".xvu"):
            continue
        r = parse(os.path.join(src, name))
        if r is None:
            unparsed += 1
            continue
        code, is_pixel, count, preamble = r
        if is_pixel:
            # A pixel shader has no vertex element table and never reaches the
            # lookup, so it is not affected and must not be rebuilt.
            pixel += 1
            continue
        if count > 0:
            # It has a real table; its semantics are true and a derived one
            # would be strictly worse.
            left_alone += 1
            continue
        if not scan(code):
            # No vfetch instructions means line 180 is never reached, so this
            # container is unaffected even though it is fetchless. This is the
            # negative control of the whole theory, and rebuilding it would
            # change a shader that was never at risk.
            no_fetch += 1
            continue

        blob = synth_xvu.build(code, is_vertex=True, preamble=preamble)
        with open(os.path.join(out, "fl_" + name), "wb") as f:
            f.write(blob)
        made += 1

    print("containers scanned      : %d" % (made + left_alone + pixel + no_fetch + unparsed))
    print("  REBUILT (fetchless +  : %d" % made)
    print("           vfetch)")
    print("  left alone, has table : %d" % left_alone)
    print("  left alone, pixel     : %d" % pixel)
    print("  left alone, no vfetch : %d  (never reaches the lookup)" % no_fetch)
    print("  unparseable           : %d" % unparsed)
    print("written: %s" % out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
