#!/usr/bin/env python3
"""How many of NG2's vertex shaders declare NO vertex fetches?

The Fable II session measured five genuinely fetchless vertex shaders among
their 76 real vertex containers, and found that ALLOWING such draws through
their renderer crashes it - so the refusal is their draw path's rule, not the
hardware's, and it is load-bearing. Their A/B, one binary one cvar:

    allow_fetchless=false   0 crashes, 190,002 replayed draws
    allow_fetchless=true    1 crash,    18,724 replayed draws

Whether NG2 has any is a question about NG2, not about the platform. Five rules
this week were generalised from one title's containers and four were wrong on
the other's, so this is measured here before stage 2b's draw path assumes
anything about bound streams.

A VertexShader extends Shader (24 bytes) with field18, vertexElementCount, then
the element array. vertexElementCount == 0 is a shader the game ships with no
vertex fetches at all.

Usage: check_fetchless.py <container dir>
"""
import os
import struct
import sys


def read_vertex_shader(path):
    """Return (is_vertex, vertexElementCount) or None if not parseable."""
    d = open(path, "rb").read()
    if len(d) < 0x24:
        return None
    flags = struct.unpack_from(">I", d, 0)[0]
    if (flags & 0xFFFFFF00) != 0x102A1100:
        return None
    # isPixelShader = (flags & 1) == 0, so a VERTEX shader has the low bit set.
    is_vertex = (flags & 1) != 0
    if not is_vertex:
        return (False, None)
    shoff = struct.unpack_from(">I", d, 0x18)[0]
    # Shader is 24 bytes; VertexShader adds field18 then vertexElementCount.
    at = shoff + 24 + 4
    if at + 4 > len(d):
        return (True, None)
    count = struct.unpack_from(">I", d, at)[0]
    # A wildly large count means the layout assumption is wrong for this
    # container, not that the shader has a million elements. Say so rather than
    # counting it as data.
    if count > 64:
        return (True, -1)
    return (True, count)


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else r"D:\ng2_frameinterp\shaders\xvu"
    files = sorted(f for f in os.listdir(root) if f.endswith(".xvu"))

    vertex = pixel = unparsed = implausible = 0
    fetchless = []
    by_count = {}
    for name in files:
        r = read_vertex_shader(os.path.join(root, name))
        if r is None:
            unparsed += 1
            continue
        is_vertex, count = r
        if not is_vertex:
            pixel += 1
            continue
        vertex += 1
        if count is None or count < 0:
            implausible += 1
            continue
        by_count[count] = by_count.get(count, 0) + 1
        if count == 0:
            fetchless.append(name)

    print("containers: %d files -> %d vertex, %d pixel, %d unparsed"
          % (len(files), vertex, pixel, unparsed))
    if implausible:
        print("  %d vertex containers whose element count did not parse plausibly" % implausible)
    print("vertexElementCount distribution:")
    for k in sorted(by_count):
        print("   %2d elements -> %4d shaders%s" % (k, by_count[k], "   <-- FETCHLESS" if k == 0 else ""))
    print()
    if fetchless:
        print("FETCHLESS VERTEX SHADERS: %d of %d (%.1f%%)"
              % (len(fetchless), vertex, 100.0 * len(fetchless) / vertex))
        print("  so a stage 2b draw path that assumes every draw binds a vertex")
        print("  stream WILL meet these, and must reject or handle them by design")
        for n in fetchless[:8]:
            print("   ", n)
        if len(fetchless) > 8:
            print("    ... and %d more" % (len(fetchless) - 8))
    else:
        print("NO FETCHLESS VERTEX SHADERS among NG2's containers.")
        print("  That is NOT permission to assume every draw binds a stream - the")
        print("  33 UNCONTAINED shaders are not measured here, and they are the ones")
        print("  with no declared element table to read.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
